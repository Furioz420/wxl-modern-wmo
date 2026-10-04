// Native modern-WMO reader: the shared runtime state (per-root verdict, four-layer side data, counters).
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

#include "WmoNativeShared.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace wxl::runtime::wmonative::detail
{
    std::atomic<uint32_t> g_rootsModern{0}, g_rootsStock{0}, g_groupsModern{0}, g_groupsFailed{0};
    std::atomic<uint32_t> g_texResolved{0}, g_texUnresolved{0};
    std::atomic<uint32_t> g_parkedDoodads{0}, g_parkedLiquids{0}, g_parkedIndex32{0}, g_unknownChunks{0};
    std::atomic<uint32_t> g_shaderRemapped{0}, g_parkedNoUv{0};
    std::atomic<uint32_t> g_shaderToTwoLayer{0}, g_shaderToEnv{0}, g_shaderToSingle{0};
    std::atomic<uint32_t> g_materialIdsMoved{0}, g_materialOutOfRange{0};
    std::atomic<uint32_t> g_batchCullBypassed{0};
    std::atomic<uint32_t> g_shaderSeen{0};
    std::atomic<uint32_t> g_vertexColorFixed{0};
    std::atomic<uint32_t> g_uvTransformed{0};
    std::atomic<uint32_t> g_layeredGroups{0}, g_layeredMaterials{0};
    std::atomic<bool>     g_installed{false};
    std::atomic<bool>     g_shaderRemapEnabled{true};
    std::atomic<int>      g_uvMode{0};

    const void* g_curBatch = nullptr;

    std::mutex g_layeredMutex;
    std::unordered_map<void*, std::unordered_map<uint32_t, LayeredMaterial>> g_layeredByRoot;
    std::unordered_map<void*, LayeredGroup> g_layeredByGroup;

    namespace
    {
        std::mutex g_rootMutex;
        std::unordered_map<void*, bool> g_rootIsModern;
    }

    void RecordRootKind(void* root, bool modern)
    {
        std::lock_guard<std::mutex> lock(g_rootMutex);
        g_rootIsModern[root] = modern;
    }

    bool RootIsModern(void* root)
    {
        std::lock_guard<std::mutex> lock(g_rootMutex);
        auto it = g_rootIsModern.find(root);
        return it != g_rootIsModern.end() && it->second;
    }

    void DropLayeredRoot(void* root)
    {
        std::lock_guard<std::mutex> lock(g_layeredMutex);
        g_layeredByRoot.erase(root);
    }

    void DropLayeredGroup(void* group)
    {
        std::lock_guard<std::mutex> lock(g_layeredMutex);
        g_layeredByGroup.erase(group);
    }
}
