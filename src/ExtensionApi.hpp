// wxl-modern-wmo: the extension-wide service table pointer and hook-install convenience, shared by every
// translation unit in this DLL.
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

#include "wxl/FdidApi.h"
#include "wxl/LoadPoolApi.h"
#include "wxl/M2DrawApi.h"
#include "wxl/PluginApi.h"

#include <cstdint>

/// See wxl-adt's ExtensionApi.hpp for the reasoning behind every pattern here: the core hands this
/// pointer to WXL_Load once and it lives for the process lifetime, so every detour installed later
/// reaches it through here rather than threading an `api` parameter through the call chain.
namespace wxl_modern_wmo
{
    extern const WXL_Api* g_api;

    /// True builds the whole native WMO pipeline in; false is the single point that would leave the
    /// client on its stock WMO reader.
    inline constexpr bool kEnabled = true;

    /// Keep WMO root/group walking on the client thread for now.  The load-pool path is valid for the
    /// current retail data set, but custom maps can stream roots and groups through legacy lifetime
    /// assumptions that the deferred finalizer does not yet model.  This preserves the modern
    /// tag-driven reader while restoring the pre-extension finalization order.
    inline constexpr bool kAsyncEnabled = false;

    // --- wxl-db2's FileDataID resolver, fetched lazily (extensions load alphabetically; wxl-db2 may
    // not have published yet when THIS extension's WXL_Load runs, but always has by the time any real
    // material load asks for a path). ---------------------------------------------------------------
    extern const WXL_FdidApi* g_fdid;

    inline const WXL_FdidApi* Fdid()
    {
        if (!g_fdid)
            g_fdid = static_cast<const WXL_FdidApi*>(g_api->GetInterface("wxl.fdid", WXL_FDID_API_VERSION));
        return g_fdid;
    }

    inline const char* ResolveTexture(uint32_t fileDataId)
    {
        const WXL_FdidApi* fdid = Fdid();
        return fdid ? fdid->ResolveTexture(fileDataId) : nullptr;
    }

    // --- wxl-modern-m2's one-shot draw interceptor, same lazy-fetch reasoning (wxl-modern-m2 loads after wxl-modern-wmo
    // alphabetically too). Only the four-layer material path (LayeredShader.cpp) needs this. ---------
    extern const WXL_M2DrawApi* g_m2draw;

    inline const WXL_M2DrawApi* M2Draw()
    {
        if (!g_m2draw)
            g_m2draw = static_cast<const WXL_M2DrawApi*>(g_api->GetInterface("wxl.m2draw", WXL_M2DRAW_API_VERSION));
        return g_m2draw;
    }

    // --- wxl-engine-reforged's background job pool for load-time fixup work. Same lazy-fetch reasoning
    // (wxl-modern-wmo loads before wxl-engine-reforged alphabetically). A caller that gets null back
    // (extension absent/disabled) falls back to running its stage1/stage2 inline itself. -------------
    extern const WXL_LoadPoolApi* g_loadPool;

    inline const WXL_LoadPoolApi* LoadPool()
    {
        if (!g_loadPool)
            g_loadPool = static_cast<const WXL_LoadPoolApi*>(
                g_api->GetInterface("wxl.loadpool", WXL_LOADPOOL_API_VERSION));
        return g_loadPool;
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttach: detour and original share one function
     *        type, deduced, so wiring a hook to the wrong original no longer compiles. Mirrors
     *        wxl::hook::Install's own convenience overload, which this extension cannot link against.
     */
    template <class Fn>
    inline int HookAttach(const char* name, uintptr_t target, Fn* detour, Fn** original,
                          int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttach(name, target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original), priority);
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttachByName: same deduced-Fn safety as
     *        HookAttach, but resolves the target by name through the core's own hook-point table
     *        (see HookPoints.cpp) instead of an offsets/ address -- this extension needs no offsets/
     *        header just to name a hook target.
     */
    template <class Fn>
    inline int HookAttachByName(const char* pointName, Fn* detour, Fn** original,
                                int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttachByName(pointName, reinterpret_cast<void*>(detour),
                                       reinterpret_cast<void**>(original), priority);
    }

    // Per-file installers, called from WXL_Load in Module.cpp.
    bool InstallCollisionRay();    // Invalid ray rejection before native candidate collection
    bool InstallWmoNative();       // WmoNative.cpp (root/group walk + material + cull, gated by kEnabled)
    bool InstallWmoSpawn();        // WmoSpawn.cpp (unconditional)
    bool InstallWmoGroupParse();   // GroupParse.cpp (unconditional)
    bool InstallOutdoorGate();     // OutdoorGate.cpp (gated by kEnabled)
    bool InstallCompositeShader(); // CompositeShader.cpp (gated by kEnabled)
    bool InstallWmoAsync();        // WmoAsync.cpp (background root/group walk, gated by kEnabled)
}

// common/Log.hpp's WLOG_* macros need common/Log.cpp linked in, which is core/host/patcher-only; an
// extension has no such object file, so these route the same call-site syntax through WXL_Api::Log.
#define WLOG_TRACE(...) ::wxl_modern_wmo::g_api->Log(WXL_LOG_TRACE, "wxl-modern-wmo", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_modern_wmo::g_api->Log(WXL_LOG_DEBUG, "wxl-modern-wmo", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_modern_wmo::g_api->Log(WXL_LOG_INFO,  "wxl-modern-wmo", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_modern_wmo::g_api->Log(WXL_LOG_WARN,  "wxl-modern-wmo", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_modern_wmo::g_api->Log(WXL_LOG_ERROR, "wxl-modern-wmo", __VA_ARGS__)
