// Native modern-WMO reader: the feature entry -- installs the walkers/material/cull and the query surface.
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

// The reader is split by responsibility: WmoState (shared state), WmoLoad (root/group walkers + cull),
// WmoMaterial (FileDataID textures + shader remap), WmoVertexColor (the MOCV alpha contract). This unit
// wires them together as one feature and exposes the read-only query surface the overlay/effects use.
//
// The MOBA material id is NOT handled on the render path here. A modern batch carries it as a u16 at
// +0x0A with bit 0x02 set at +0x16, while the client reads a byte at +0x17 (zero in these files); the
// group walk copies the modern u16 into that byte, so every batch resolves its real material.

#include "WmoNative.hpp"
#include "WmoNativeShared.hpp"

#include "game/Wmo.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace
{
    using namespace wxl::runtime::wmonative::detail;
}

// ---------------------------------------------------------------- public query surface
namespace wxl::runtime::wmonative
{
    Stats GetStats()
    {
        Stats s{};
        s.rootsModern           = detail::g_rootsModern.load(std::memory_order_relaxed);
        s.rootsStock            = detail::g_rootsStock.load(std::memory_order_relaxed);
        s.groupsModern          = detail::g_groupsModern.load(std::memory_order_relaxed);
        s.groupsFailed          = detail::g_groupsFailed.load(std::memory_order_relaxed);
        s.texturesResolved      = detail::g_texResolved.load(std::memory_order_relaxed);
        s.texturesUnresolved    = detail::g_texUnresolved.load(std::memory_order_relaxed);
        s.parkedDoodadDefs      = detail::g_parkedDoodads.load(std::memory_order_relaxed);
        s.parkedLiquids         = detail::g_parkedLiquids.load(std::memory_order_relaxed);
        s.parkedIndex32         = detail::g_parkedIndex32.load(std::memory_order_relaxed);
        s.unknownChunks         = detail::g_unknownChunks.load(std::memory_order_relaxed);
        s.shaderRemapped        = detail::g_shaderRemapped.load(std::memory_order_relaxed);
        s.shaderToTwoLayer      = detail::g_shaderToTwoLayer.load(std::memory_order_relaxed);
        s.shaderToEnv           = detail::g_shaderToEnv.load(std::memory_order_relaxed);
        s.shaderToSingle        = detail::g_shaderToSingle.load(std::memory_order_relaxed);
        s.parkedNoUvGroups      = detail::g_parkedNoUv.load(std::memory_order_relaxed);
        s.materialIdsMoved      = detail::g_materialIdsMoved.load(std::memory_order_relaxed);
        s.materialIdsOutOfRange = detail::g_materialOutOfRange.load(std::memory_order_relaxed);
        s.batchCullBypassed     = detail::g_batchCullBypassed.load(std::memory_order_relaxed);
        s.vertexColorFixed      = detail::g_vertexColorFixed.load(std::memory_order_relaxed);
        s.uvTransformed         = detail::g_uvTransformed.load(std::memory_order_relaxed);
        s.layeredGroups         = detail::g_layeredGroups.load(std::memory_order_relaxed);
        s.layeredMaterials      = detail::g_layeredMaterials.load(std::memory_order_relaxed);
        return s;
    }

    bool Enabled() { return wxl_modern_wmo::kEnabled; }

    bool Installed() { return detail::g_installed.load(std::memory_order_relaxed); }

    bool IsModernRoot(void* root) { return root && detail::RootIsModern(root); }

    bool ShaderRemapEnabled() { return detail::g_shaderRemapEnabled.load(std::memory_order_relaxed); }
    void SetShaderRemapEnabled(bool on) { detail::g_shaderRemapEnabled.store(on, std::memory_order_relaxed); }

    int  UvMode() { return detail::g_uvMode.load(std::memory_order_relaxed); }
    void SetUvMode(int mode) { detail::g_uvMode.store(mode, std::memory_order_relaxed); }

    LayeredMaterial* FindLayeredMaterial(void* root, uint32_t materialIndex)
    {
        std::lock_guard<std::mutex> lock(detail::g_layeredMutex);
        auto rootIt = detail::g_layeredByRoot.find(root);
        if (rootIt == detail::g_layeredByRoot.end()) return nullptr;
        auto matIt = rootIt->second.find(materialIndex);
        return matIt == rootIt->second.end() ? nullptr : &matIt->second;
    }

    bool GetLayeredGroup(void* group, LayeredGroup& out)
    {
        std::lock_guard<std::mutex> lock(detail::g_layeredMutex);
        auto it = detail::g_layeredByGroup.find(group);
        if (it == detail::g_layeredByGroup.end()) return false;
        out = it->second;
        return true;
    }

    uint32_t BatchMaterialIndex(const void* mobaRecord)
    {
        return wxl::game::wmo::GetBatchMaterialIndex(mobaRecord);
    }

    const void* CurrentBatch() { return detail::g_curBatch; }
    void ClearCurrentBatch() { detail::g_curBatch = nullptr; }
}

namespace wxl_modern_wmo
{
    bool InstallWmoNative()
    {
        using namespace wxl::runtime::wmonative::detail;
        const bool rootOk  = InstallRootWalk();
        const bool groupOk = InstallGroupWalk();
        const bool matOk   = InstallMaterial();
        const bool cullOk  = InstallCull();

        // All or nothing: a half-installed reader would classify roots as modern and then hand them to
        // a walker that never ran, which is the one failure mode worse than staying stock.
        const bool ok = rootOk && groupOk && matOk && cullOk;
        g_installed.store(ok, std::memory_order_relaxed);
        if (!ok)
            WLOG_ERROR("wmo-native: install failed (root=%d group=%d material=%d cull=%d), staying stock",
                       rootOk ? 1 : 0, groupOk ? 1 : 0, matOk ? 1 : 0, cullOk ? 1 : 0);
        return ok;
    }
}
