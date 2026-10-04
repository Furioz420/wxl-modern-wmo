// Background root/group walk orchestration: moves WalkRootModern/WalkGroupModern (WmoLoad.cpp) off the
// main thread onto wxl.loadpool, the same win wxl-adt already got for ParseAndPatch.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Root/group finalize is one fused native step (materials/MOHD copy/group allocation for a root;
// CreateMaterial GPU binds/antiportal/bbox/two-UV for a group), with no separately-callable stage the
// way ADT's tile finalize has, so this design never reimplements that remainder. It submits stage1
// (WalkModernRoot/WalkModernGroup, WmoLoad.cpp) to the background,
// arms a one-shot "already walked" marker WmoLoad.cpp's own hkRootWalk/hkGroupWalk consume, then lets
// stage2 call the TRUE, unmodified native completion callback on the main thread -- which reaches the
// walker hook (now a no-op for this object) and runs its own remainder exactly as it always has.
//
// AsyncGate::walking is the ONLY condition anything ever blocks on, and it is cleared exclusively by the
// background WORKER thread (RootStage1/GroupStage1) -- never by anything running on the main thread. That
// is deliberate: stage2 (the deferred native finalize) only ever runs on the main thread, so a main-thread
// caller blocking on stage2 would deadlock the one thread that could ever run it (the same trap ADT's
// split-tile reader avoids for the same reason). The two main-thread consumers that need stage2 to
// have definitely happened -- FreeMapObj/FreeMapObjGroup (about to free the object) and WaitLoad/
// WaitLoadGroup (a caller wants "fully usable now") -- resolve it themselves once stage1 is known done:
// the destructors CANCEL a still-pending stage2 (nothing left to finalize into), WaitLoad RUNS it right
// there on the calling thread if the pool hasn't drained it yet. Either way stage2 executes AT MOST once,
// enforced by consuming AsyncGate::finalizeFn under the same lock that decides who runs it.
//
// Two more things this file has to hold that ADT's tile reader didn't need:
//  - The native "still loading" markers (root+0x1DC, group+0x194) must be left exactly as the async read
//    left them for the whole background window -- not cleared early, not faked non-null with a
//    substitute object (the client's own async-wait helper keys off this field directly, so a substitute
//    breaks its internal reentrancy guard). Achieved simply by not calling the native completion until
//    stage2.
//  - A purge can free the root/group buffer out from under a still-writing background walk with zero
//    guard from native code (the client's own teardown path only cancels the OS-level read state, already
//    false by the time any of this runs) -- FreeMapObj/FreeMapObjGroup wait out `walking` first.

#include "../ExtensionApi.hpp"
#include "WmoNativeShared.hpp"

#include "engine/events/Event.hpp"
#include "game/Binding.hpp"
#include "offsets/game/WMO.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace off = wxl::offsets::game::wmo;
namespace wld = wxl::offsets::game::world;
namespace ev  = wxl::events;

namespace wxl::runtime::wmonative::detail
{
    namespace
    {
        /**
         * @brief Per-object background-walk tracking. One instance per root/group ever submitted through
         *        Submit*Complete; never erased (a recycled pool slot's stale fields are fully overwritten
         *        by the next SubmitRootComplete/SubmitGroupComplete call for that address, and nothing
         *        ever erases the map entry itself, so a pointer into it stays valid for the process
         *        lifetime -- no "erase while another thread still holds a reference" hazard to avoid).
         */
        struct AsyncGate
        {
            std::mutex              mutex;
            std::condition_variable cv;         ///< signals `walking` only -- see file header comment
            bool  walking    = false;           ///< stage1 actively running on a worker thread right now
            bool  prewalked  = false;           ///< stage1 done; the resume hook consumes this once
            bool  pending    = false;           ///< finalize has not run (and not been cancelled) yet
            void (__cdecl* finalizeFn)(void*) = nullptr; ///< consumed by whichever caller runs it first
        };

        std::mutex                                            g_gateMapMutex; // guards the two maps only
        std::unordered_map<void*, std::unique_ptr<AsyncGate>> g_rootGates;
        std::unordered_map<void*, std::unique_ptr<AsyncGate>> g_groupGates;

        AsyncGate& GateFor(std::unordered_map<void*, std::unique_ptr<AsyncGate>>& map, void* key)
        {
            std::lock_guard<std::mutex> lock(g_gateMapMutex);
            auto& slot = map[key];
            if (!slot) slot = std::make_unique<AsyncGate>();
            return *slot;
        }

        AsyncGate* FindGate(std::unordered_map<void*, std::unique_ptr<AsyncGate>>& map, void* key)
        {
            std::lock_guard<std::mutex> lock(g_gateMapMutex);
            auto it = map.find(key);
            return it == map.end() ? nullptr : it->second.get();
        }

        bool TakePrewalked(std::unordered_map<void*, std::unique_ptr<AsyncGate>>& map, void* key)
        {
            AsyncGate* gate = FindGate(map, key);
            if (!gate) return false;
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (!gate->prewalked) return false;
            gate->prewalked = false;
            return true;
        }

        /// __try/__except cannot coexist with a C++ object needing unwinding in the SAME function (MSVC
        /// C2712) -- SubmitRootComplete has lock_guards further down, so the guarded classify call is
        /// pulled out into this small, unwinding-free function instead.
        bool ClassifyRootBufferGuarded(uint8_t* base, uint32_t size)
        {
            __try { return ClassifyRootBuffer(base, size); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        /// Runs `gate.finalizeFn(obj)` exactly once -- a no-op if it already ran, or was cancelled by a
        /// racing destructor. Callers (RootStage2/GroupStage2, or an impatient WaitLoad/WaitLoadGroup)
        /// must NOT hold `gate.mutex` and must already know stage1 (`walking`) has finished.
        void RunFinalizeOnce(AsyncGate& gate, void* obj)
        {
            void (__cdecl* fn)(void*) = nullptr;
            {
                std::lock_guard<std::mutex> lock(gate.mutex);
                if (!gate.pending) return;
                fn = gate.finalizeFn;
                gate.finalizeFn = nullptr;
                gate.pending = false;
            }
            if (fn) fn(obj);
        }

        /// Waits out a background walk in flight for `obj`, then cancels a still-pending finalize instead
        /// of running it -- the object is about to be freed, so its native remainder (materials/MOHD
        /// copy/group allocation, or CreateMaterial/antiportal/bbox for a group) must never run. Only
        /// blocks on `walking`, cleared by the worker thread on its own -- never deadlocks the caller.
        void CancelPending(AsyncGate* gate)
        {
            if (!gate) return;
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->cv.wait(lock, [&] { return !gate->walking; });
            if (gate->pending)
            {
                gate->finalizeFn = nullptr;
                gate->pending    = false;
                gate->prewalked  = false; // finalize (and hkRootWalk's consume of it) will never run now
            }
        }

        /// Waits out a background walk in flight for `obj`, then makes sure finalize has run -- either it
        /// already did (pool drain beat this caller to it, RunFinalizeOnce is then a no-op) or it hasn't,
        /// in which case this call runs it right here rather than blocking on the main-thread-only drain
        /// (see file header comment for why blocking on that specifically would deadlock).
        void EnsureFinalized(AsyncGate* gate, void* obj)
        {
            if (!gate) return;
            {
                std::unique_lock<std::mutex> lock(gate->mutex);
                gate->cv.wait(lock, [&] { return !gate->walking; });
            }
            RunFinalizeOnce(*gate, obj);
        }

        /// Runs on a wxl.loadpool worker thread (or inline, see SubmitRootComplete). The only background
        /// access to the root's fields; arms `prewalked` and clears `walking` -- the sole condition
        /// anything in this file ever blocks on -- for the main thread to consume.
        void __cdecl RootStage1(void* root)
        {
            WalkModernRoot(root);
            AsyncGate& gate = GateFor(g_rootGates, root);
            {
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.walking   = false;
                gate.prewalked = true;
            }
            gate.cv.notify_all();
        }

        /// Runs on the main thread (the wxl.loadpool per-frame drain, or inline). Calls the TRUE native
        /// completion, unmodified -- see this file's header comment for why that, not a hand-written
        /// reproduction, is what "finalize" means here. A no-op if a racing destructor already cancelled
        /// it (RunFinalizeOnce's own check).
        void __cdecl RootStage2(void* root)
        {
            RunFinalizeOnce(GateFor(g_rootGates, root), root);
        }

        void __cdecl GroupStage1(void* group)
        {
            auto* base = reinterpret_cast<uint8_t*>(static_cast<off::Group*>(group)->groupBuffer);
            WalkModernGroup(group, base + off::kGroupWalkCursorOffset);
            AsyncGate& gate = GateFor(g_groupGates, group);
            {
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.walking   = false;
                gate.prewalked = true;
            }
            gate.cv.notify_all();
        }

        void __cdecl GroupStage2(void* group)
        {
            RunFinalizeOnce(GateFor(g_groupGates, group), group);
        }

        /// Drains one tick of the native completion queue (kAsyncServiceQueues) -- the same queue the
        /// client's own async-wait helper loops on internally, but reached here WITHOUT ever going through
        /// that helper, so its internal reentrancy guard is never touched. Already reentrancy-guarded
        /// against nested pumps by Streaming.cpp's own AsyncDrain hook on this same address (depth-tracked
        /// there), since hooking patches the address itself -- any caller reaching it, including this one,
        /// is redirected through that hook automatically.
        void PumpAsyncCompletionsOnce()
        {
            wxl::game::Native<wld::AsyncServiceQueuesFn>(wld::kAsyncServiceQueues)(0, 0);
        }

        // ------------------------------------------------------------------ new hook trampolines
        off::Wmo_GroupCompleteFn   g_origGroupComplete   = nullptr;
        off::Wmo_FreeMapObjFn      g_origFreeMapObj      = nullptr;
        off::Wmo_FreeMapObjGroupFn g_origFreeMapObjGroup = nullptr;
        off::Wmo_WaitLoadFn        g_origWaitLoad        = nullptr;
        off::Wmo_WaitLoadGroupFn   g_origWaitLoadGroup   = nullptr;
        off::Wmo_UpdateMaterialsFn g_origUpdateMaterials = nullptr;

        /// The group completion callback, hooked directly (no other extension owns this address): submits
        /// the group's walk exactly as hkWmoRootComplete (WmoSpawn.cpp) does for a root.
        void __cdecl hkGroupComplete(void* group)
        {
            SubmitGroupComplete(group, g_origGroupComplete);
        }

        void __cdecl hkFreeMapObj(void* root)
        {
            CancelPending(FindGate(g_rootGates, root));
            g_origFreeMapObj(root);
        }

        void __cdecl hkFreeMapObjGroup(void* group)
        {
            CancelPending(FindGate(g_groupGates, group));
            g_origFreeMapObjGroup(group);
        }

        /// Blocking wait for a root: a caller wants "block until this root is genuinely usable". NEVER
        /// calls the native wait helper, gated or not -- confirmed off a live-instrumented crash that the
        /// client's own wait loop assumes its wait call's return means fully finalized, because natively
        /// the completion callback IS the finalize, run inline. Ours isn't: if this hook is entered before
        /// a gate exists (the root's read is still in flight) and falls through to native, native's first
        /// wait call blocks normally, but the read completes DURING that block, our completion hook fires
        /// reentrantly from inside it, arms the gate, queues the background walk and returns WITHOUT
        /// finalizing -- so root+0x1DC is still non-null when native's loop re-checks it, and native calls
        /// its wait helper a second time on an object now already marked handled, hitting the exact
        /// early-return that leaks the guard (a third call, from anywhere, then hits the fatal assert). No
        /// gating fix at this hook's own entry point can close that: the leak happens one call frame below,
        /// inside native code we no longer control once we've fallen through once.
        ///
        /// Fixed by never falling through at all: spin on wxl's own state, pumping the completion drain
        /// directly (PumpAsyncCompletionsOnce -- never the native wait helper, so its reentrancy guard is
        /// never touched here) until either a gate appears (then EnsureFinalized) or the handle clears on
        /// its own (the completion ran synchronously without ever creating a gate -- a classic root, or
        /// one that finished between iterations).
        void __fastcall hkWaitLoad(void* root, void* edx)
        {
            for (;;)
            {
                if (AsyncGate* gate = FindGate(g_rootGates, root))
                {
                    EnsureFinalized(gate, root);
                    return;
                }
                if (!*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + off::kOffRootAsyncHandle))
                    return; // nothing in flight and nothing wxl is tracking -- matches native's idle case
                PumpAsyncCompletionsOnce();
                ::Sleep(1);
            }
        }

        /// Group-side twin of hkWaitLoad, same reasoning (the native group wait helper loops on
        /// group+0x194 the same way the root wait helper loops on root+0x1DC). If the ROOT itself isn't finalized yet
        /// (groupIndex not yet valid against groupCount -- the array wxl's own deferred finalize
        /// populates), resolve the root first through the identical never-touch-native path, then retry.
        void __fastcall hkWaitLoadGroup(void* root, void* edx, int groupIndex)
        {
            auto* r = static_cast<off::Root*>(root);
            for (;;)
            {
                if (static_cast<uint32_t>(groupIndex) < r->groupCount)
                {
                    void* group = r->groupArray[groupIndex];
                    if (AsyncGate* gate = FindGate(g_groupGates, group))
                    {
                        EnsureFinalized(gate, group);
                        return;
                    }
                    if (!*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(group) + off::kOffGroupAsyncHandle))
                        return;
                    PumpAsyncCompletionsOnce();
                    ::Sleep(1);
                    continue;
                }
                hkWaitLoad(root, edx);
                if (static_cast<uint32_t>(groupIndex) >= r->groupCount)
                    return; // still out of range after the root resolved -- caller error, nothing to wait for
            }
        }

        /// Per-frame material update: skip, don't wait -- see IsRootPending's doc comment for why this one
        /// runs every frame for every root and must never block.
        void __fastcall hkUpdateMaterials(void* root, void* edx)
        {
            if (IsRootPending(root)) return;
            g_origUpdateMaterials(root, edx);
        }
    }

    void SubmitRootComplete(void* root, RootFinalizeFn finalize)
    {
        auto* r = static_cast<off::Root*>(root);
        auto* base = reinterpret_cast<uint8_t*>(r->rootBuffer);
        const uint32_t size = r->rootSize;

        const bool modern = (base && size >= 12) && ClassifyRootBufferGuarded(base, size);

        // root+0x1DC is deliberately NOT retired here: native's OWN finalize(), called below,
        // unconditionally retires root+0x1DC itself as ITS first step (with no null check on the handle)
        // -- pre-nulling it here instead makes that native call crash on a null dereference. Kept open
        // the same way group+0x194 has to stay open (see SubmitGroupComplete); hkWaitLoad/hkWaitLoadGroup
        // (WmoAsync.cpp, this file) are what keep native WaitLoad/WaitLoadGroup from ever touching it.
        if (modern && wxl_modern_wmo::kAsyncEnabled)
        {
            AsyncGate& gate = GateFor(g_rootGates, root);
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.walking    = true;
            gate.pending    = true;
            gate.finalizeFn = finalize;
        }

        // OnWmoRootLoad fires after the gate is armed: a reentrant WaitLoad/WaitLoadGroup call reaching
        // this exact root during an event subscriber's code -- none exist today, but the event is
        // exposed precisely so one might -- must find the gate already there (confirmed off a real
        // crash: without this ordering, such a call would find no gate and fall through to native).
        ev::WmoRootLoadArgs a{ root };
        wxl_modern_wmo::g_api->Emit(uint32_t(ev::Event::OnWmoRootLoad), &a);

        if (!modern || !wxl_modern_wmo::kAsyncEnabled)
        {
            // Unchanged today's behaviour: hkRootWalk (WmoLoad.cpp) does its own classify+record+drop
            // and calls the stock native walker when CreateData reaches it.  The same synchronous
            // finalize is deliberately used for modern roots when the custom-map compatibility gate
            // disables background walking; hkRootWalk still selects the tag-driven modern reader.
            finalize(root);
            return;
        }

        if (const WXL_LoadPoolApi* pool = wxl_modern_wmo::LoadPool())
            pool->Submit(&RootStage1, &RootStage2, root);
        else
        {
            RootStage1(root);
            RootStage2(root);
        }
    }

    /**
     * @brief Same shape as SubmitRootComplete, for one group. group+0x194 stays open (never retired)
     *        for the same reason root+0x1DC does now too: an area-of-interest driver (0x00795F80) checks
     *        isGroupLoaded/IsGroupLoading and issues a brand-new ReadGroup() the instant it sees a group
     *        as neither loaded (group+0x198 bit 0) nor loading (group+0x194) -- retiring the handle
     *        early would read as "abandoned" and trigger a duplicate concurrent read of the same group.
     *        hkWaitLoadGroup (WmoAsync.cpp) is what keeps a WaitLoadGroup caller from ever reaching the
     *        native wait helper on this deliberately-still-open object.
     */
    void SubmitGroupComplete(void* group, GroupFinalizeFn finalize)
    {
        if (!wxl_modern_wmo::kAsyncEnabled)
        {
            finalize(group);
            return;
        }

        void* root = static_cast<off::Group*>(group)->root;
        if (!root || !RootIsModern(root))
        {
            finalize(group);
            return;
        }

        AsyncGate& gate = GateFor(g_groupGates, group);
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.walking    = true;
            gate.pending    = true;
            gate.finalizeFn = finalize;
        }

        if (const WXL_LoadPoolApi* pool = wxl_modern_wmo::LoadPool())
            pool->Submit(&GroupStage1, &GroupStage2, group);
        else
        {
            GroupStage1(group);
            GroupStage2(group);
        }
    }

    bool TakePrewalkedRoot(void* root) { return TakePrewalked(g_rootGates, root); }
    bool TakePrewalkedGroup(void* group) { return TakePrewalked(g_groupGates, group); }

    bool IsRootPending(void* root)
    {
        AsyncGate* gate = FindGate(g_rootGates, root);
        if (!gate) return false;
        std::lock_guard<std::mutex> lock(gate->mutex);
        return gate->walking;
    }

    bool InstallAsync()
    {
        const bool group     = wxl_modern_wmo::HookAttachByName("Wmo.GroupComplete", &hkGroupComplete, &g_origGroupComplete);
        const bool freeObj    = wxl_modern_wmo::HookAttachByName("Wmo.FreeMapObj", &hkFreeMapObj, &g_origFreeMapObj);
        const bool freeGroup  = wxl_modern_wmo::HookAttachByName("Wmo.FreeMapObjGroup", &hkFreeMapObjGroup, &g_origFreeMapObjGroup);
        const bool waitRoot   = wxl_modern_wmo::HookAttachByName("Wmo.WaitLoad", &hkWaitLoad, &g_origWaitLoad);
        const bool waitGroup  = wxl_modern_wmo::HookAttachByName("Wmo.WaitLoadGroup", &hkWaitLoadGroup, &g_origWaitLoadGroup);
        const bool updateMat  = wxl_modern_wmo::HookAttachByName("Wmo.UpdateMaterials", &hkUpdateMaterials, &g_origUpdateMaterials);
        const bool ok = group && freeObj && freeGroup && waitRoot && waitGroup && updateMat;
        if (!ok)
            WLOG_WARN("wmo-async: install failed (group=%d freeObj=%d freeGroup=%d waitRoot=%d "
                      "waitGroup=%d updateMat=%d); root/group walks stay on the main thread",
                      group ? 1 : 0, freeObj ? 1 : 0, freeGroup ? 1 : 0, waitRoot ? 1 : 0,
                      waitGroup ? 1 : 0, updateMat ? 1 : 0);
        else
            WLOG_INFO("wmo-async: background root/group walk active (wxl.loadpool)");
        return true; // non-fatal: SubmitRootComplete/SubmitGroupComplete fall back to synchronous
    }
}

namespace wxl_modern_wmo
{
    bool InstallWmoAsync() { return wxl::runtime::wmonative::detail::InstallAsync(); }
}
