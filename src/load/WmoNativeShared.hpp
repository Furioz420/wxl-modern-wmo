// Native modern-WMO reader: internal contract shared across the reader's translation units.
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

#pragma once

// The reader is split by responsibility (state / load / material / vertex-colour) but is ONE subsystem
// with one shared runtime state. That state -- the per-root modern verdict, the four-layer side data,
// and the telemetry counters -- has internal linkage no single translation unit can own, so it is
// declared here (defined once in WmoState.cpp) and the raw-memory helpers every walker needs are inline.

#include "../ExtensionApi.hpp"
#include "WmoNative.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace wxl::runtime::wmonative::detail
{
    // ------------------------------------------------------------------ raw-memory helpers
    inline uint32_t Rd32(const void* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    inline void Wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }

    inline uint8_t* Field(void* obj, size_t offset) { return static_cast<uint8_t*>(obj) + offset; }

    inline void SetPtr(void* obj, size_t offset, const void* value)
    {
        Wr32(Field(obj, offset), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)));
    }

    inline void SetU32(void* obj, size_t offset, uint32_t value) { Wr32(Field(obj, offset), value); }

    inline void* GetPtr(void* obj, size_t offset)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(Rd32(Field(obj, offset))));
    }

    /**
     * @brief Walks a chunk container by tag.
     *
     * A WMO tag is stored reversed on disk, so the four bytes read back as one LE dword are exactly what
     * off::WmoTag builds -- the dword is compared directly, with no byte shuffling. The walk stops at the
     * first header that would run past `end` or whose payload does not fit, the one safety the stock
     * walker lacks.
     * @param begin  first chunk header.
     * @param end    one past the last readable byte.
     * @param fn     invoked as fn(tag, content, size) per chunk.
     */
    template <class Fn>
    void WalkChunks(uint8_t* begin, uint8_t* end, Fn&& fn)
    {
        uint8_t* p = begin;
        while (p + 8 <= end)
        {
            const uint32_t tag  = Rd32(p);
            const uint32_t size = Rd32(p + 4);
            uint8_t* content = p + 8;
            if (size > static_cast<uint32_t>(end - content))
                break;
            fn(tag, content, size);
            p = content + size;
        }
    }

    // ------------------------------------------------------------------ shared telemetry counters
    extern std::atomic<uint32_t> g_rootsModern, g_rootsStock, g_groupsModern, g_groupsFailed;
    extern std::atomic<uint32_t> g_texResolved, g_texUnresolved;
    extern std::atomic<uint32_t> g_parkedDoodads, g_parkedLiquids, g_parkedIndex32, g_unknownChunks;
    extern std::atomic<uint32_t> g_shaderRemapped, g_parkedNoUv;
    extern std::atomic<uint32_t> g_shaderToTwoLayer, g_shaderToEnv, g_shaderToSingle;
    extern std::atomic<uint32_t> g_materialIdsMoved, g_materialOutOfRange;
    extern std::atomic<uint32_t> g_batchCullBypassed;
    extern std::atomic<uint32_t> g_shaderSeen;        ///< bitmask of already-reported unsupported shader ids
    extern std::atomic<uint32_t> g_vertexColorFixed;
    extern std::atomic<uint32_t> g_uvTransformed;
    extern std::atomic<uint32_t> g_layeredGroups, g_layeredMaterials;
    extern std::atomic<bool>     g_installed;

    /// Live A/B of the family remap. True (default) maps a modern shader onto the nearest native effect
    /// keeping its second diffuse layer; false reverts to the old single-layer fallback (any unsupported
    /// id -> 0). Only affects materials not yet resolved (the handle guard in CreateMaterial).
    extern std::atomic<bool>     g_shaderRemapEnabled;
    /// Manual UV-set probe, default 0 (untouched). Modes 1..6 stay for A/B diagnostics; the real fix
    /// routes per EFFECT at draw (CompositeShader), never by rewriting the group's shared vertex data.
    extern std::atomic<int>      g_uvMode;

    // ------------------------------------------------------------------ shared runtime state
    /// One-shot latch: the batch record last kept by the per-batch cull on a modern leaf. Render is
    /// single-threaded, so a plain pointer is enough for the value itself, but the *lifetime* is not
    /// implicit -- wxl::runtime::wmonative::CurrentBatch()/ClearCurrentBatch() is the consume contract
    /// every reader must follow (see ClearCurrentBatch()'s doc comment); do not read this global
    /// directly.
    extern const void* g_curBatch;

    /// Modern verdict per root object. Rewritten on every root load, so a recycled pool slot never
    /// inherits the previous occupant's verdict.
    void RecordRootKind(void* root, bool modern);
    bool RootIsModern(void* root);

    // Four-layer (shader 23) side data, keyed by the runtime objects and rewritten on every (re)walk so a
    // recycled pool slot never serves the previous occupant's data. unordered_map is node-based, so a
    // pointer to a stored element stays valid across other inserts -- the lifetime the draw path relies on.
    extern std::mutex g_layeredMutex;
    extern std::unordered_map<void*, std::unordered_map<uint32_t, LayeredMaterial>> g_layeredByRoot;
    extern std::unordered_map<void*, LayeredGroup> g_layeredByGroup;
    // Out-of-line map drops: the walker detours use SEH (__try), which cannot coexist with objects needing
    // unwinding (lock_guard) in the same function body.
    void DropLayeredRoot(void* root);
    void DropLayeredGroup(void* group);

    // ------------------------------------------------------------------ cross-unit responsibilities
    /// WmoMaterial: maps a modern WMO pixel-shader id onto the nearest native 335 effect (0..6).
    uint32_t NativeShaderFor(uint32_t modern);
    /// WmoVertexColor: reproduces the modern MOCV alpha contract past the trans-batch vertex range.
    void NormalizeVertexColorAlpha(void* group, uint32_t groupFlags);
    /// WmoVertexColor: applies the whole load-time vertex-colour contract to a modern group (MOHD gate).
    void ApplyVertexColor(void* group);

    // Per-hook installers, each owning its own trampoline in its translation unit.
    bool InstallRootWalk();   // WmoLoad
    bool InstallGroupWalk();  // WmoLoad
    bool InstallCull();       // WmoLoad
    bool InstallMaterial();   // WmoMaterial

    // ------------------------------------------------------------------ background walk orchestration
    // WmoAsync.cpp submits WalkRootModern/WalkGroupModern (below) to wxl.loadpool, exactly as wxl-adt
    // backgrounds ParseAndPatch. Unlike ADT, the split point has to sit ABOVE the walker (at the
    // completion callback, before the native handle at root+0x1DC/group+0x194 is cleared), because the
    // walker call and the rest of the native finalize (materials, MOHD copy, group allocation / GPU
    // texture binds, antiportal, bbox) are fused into ONE native function with no clean call boundary --
    // there is no ADT-style separately-callable "TileAreaCreate" to invoke once ready. So stage2 does not
    // reproduce that remainder by hand: it simply lets the ORIGINAL, unmodified completion callback run,
    // and WalkRootModern/WalkGroupModern's own hook (hkRootWalk/hkGroupWalk) becomes a one-shot no-op for
    // an object stage1 already walked, so the native call reaches its own remainder exactly as it always
    // has -- adapt native's own code by resuming it, never re-implement it.

    // WalkRootModern/WalkGroupModern themselves, exposed from WmoLoad.cpp so WmoAsync.cpp's background
    // stage1 can call them directly (bypassing the hooks, which are the RESUME point, not the trigger).
    // Both are SEH-guarded internally; a fault classifies as "walk failed", never a crash.
    bool ClassifyRootBuffer(uint8_t* base, uint32_t size);
    void WalkModernRoot(void* root);                  // caller must already know the root is modern
    void WalkModernGroup(void* group, void* cursor);  // caller must already know the root is modern

    // Consumed by hkRootWalk/hkGroupWalk: true (and clears the marker) exactly once per background walk
    // that already ran for this object -- the hook must skip its own work and return immediately, letting
    // the native caller's remainder run against the fields stage1 already populated. False for every
    // other call (stock objects, or an object never submitted through Submit*Complete below).
    bool TakePrewalkedRoot(void* root);
    bool TakePrewalkedGroup(void* group);

    using RootFinalizeFn  = void(__cdecl*)(void* root);
    using GroupFinalizeFn = void(__cdecl*)(void* group);

    /// Classifies `root`; a stock root finalizes synchronously and immediately (calls `finalize(root)`
    /// before returning -- byte-for-byte today's behaviour). A modern root's walk is submitted to
    /// wxl.loadpool (or run inline if the pool is unavailable/disabled): stage1 populates the root's
    /// fields directly and arms TakePrewalkedRoot's one-shot marker; stage2 then calls `finalize` -- the
    /// TRUE, unmodified native completion -- which clears the native async handle, runs CreateData (whose
    /// own call into the walker now no-ops), and does the rest of native's own root finalize exactly as
    /// it always has. Call from the root's completion-callback hook, BEFORE the native completion (which
    /// would clear root+0x1DC) is allowed to run.
    void SubmitRootComplete(void* root, RootFinalizeFn finalize);
    /// Same shape as SubmitRootComplete, for one group.
    void SubmitGroupComplete(void* group, GroupFinalizeFn finalize);

    /// True while `root` has a background walk (stage1) ACTIVELY RUNNING on a worker thread right now --
    /// deliberately narrower than "any job outstanding": stage1 is the only phase that writes the fields
    /// UpdateMaterials reads (root+0x19C/root+0x160), so once it finishes those fields are stable even
    /// if stage2 (the deferred native finalize) hasn't run yet. Used by UpdateMaterials' torn-read guard,
    /// which must SKIP -- never block, it runs every frame for every tracked root -- rather than wait.
    ///
    /// Deliberately NOT exposed as a general "wait for everything" primitive: the destructor and
    /// WaitLoad detours (WmoAsync.cpp) need different, incompatible behaviour once stage1 is done --
    /// the destructor must CANCEL a still-pending stage2 (the root is about to be freed), WaitLoad must
    /// RUN it right now on the calling thread if the pool hasn't drained it yet (blocking on stage2
    /// itself would deadlock: stage2 only ever runs on the main thread, the same thread every WaitLoad
    /// caller is on) -- so that logic lives inline in WmoAsync.cpp's own hook bodies, not here.
    bool IsRootPending(void* root);

    bool InstallAsync(); // WmoAsync
}
