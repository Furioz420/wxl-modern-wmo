// Native modern-WMO reader: the load-time MOCV vertex-colour contract (the MOHD-gated alpha rewrite).
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

#include "game/Binding.hpp"
#include "offsets/game/WMO.hpp"

#include <atomic>
#include <cstdint>

namespace off = wxl::offsets::game::wmo;

namespace wxl::runtime::wmonative::detail
{
    /// Load-time MOCV alpha contract of a modern group (root sets MOHD 0x08, do_not_fix_vertex_color
    /// _alpha -- every modern file observed). The modern engine ships MOCV RGB raw and rewrites ONLY the
    /// alpha at load: opaque for an exterior group, zero for an interior one, past the trans-batch
    /// vertex range. Trans-range alpha keeps its authored value -- it weights this client's own
    /// two-pass interior/exterior cross-fade.
    ///
    /// RGB is deliberately untouched. The combine this client binds for these groups was read back
    /// live from the bound shaders: `oD0 = light + MOCV` in the VS, `out = tex * oD0 * 2` in the PS
    /// -- the vertex colour is ADDED to the scene light and the product doubled, the same structure
    /// (and the same weight, raw) the modern shaders give it. The lit/unlit material fork the modern
    /// engine applies is already this client's own per-batch lighting mode (kMomtFlagUnlit), so no
    /// per-vertex or per-batch data split is needed here.
    void NormalizeVertexColorAlpha(void* group, uint32_t groupFlags)
    {
        auto* mocv = static_cast<uint8_t*>(GetPtr(group, off::kGroupSlots[8].ptrField));
        const uint32_t count = Rd32(Field(group, off::kGroupSlots[8].countField));
        if (!mocv || count == 0)
            return;

        // First vertex past the trans batches (the head of the shared vertex range).
        uint32_t transEnd = 0;
        auto* batches = static_cast<off::MobaRecord*>(GetPtr(group, off::kGroupSlots[5].ptrField));
        const uint32_t batchCount = Rd32(Field(group, off::kGroupSlots[5].countField));
        const uint16_t transBatches = static_cast<off::Group*>(group)->transBatchCount;
        if (batches && transBatches != 0 && transBatches <= batchCount)
        {
            const uint16_t maxIndex = batches[transBatches - 1].maxIndex;
            transEnd = static_cast<uint32_t>(maxIndex) + 1;
            if (transEnd > count) transEnd = count;
        }

        const uint8_t alpha = (groupFlags & off::kGroupFlagExterior) ? 0xFF : 0x00;
        for (uint32_t v = transEnd; v < count; ++v)
            mocv[static_cast<size_t>(v) * 4u + 3] = alpha; // B,G,R,A stride 4
    }

    /// Applies the whole load-time vertex-colour contract to a modern group. Under MOHD 0x08 (set by
    /// every modern root) the modern engine ships MOCV RGB raw and rewrites only the alpha; its shaders
    /// -- not a CPU fix -- decide per MATERIAL whether the vertex colour multiplies the texture (unlit)
    /// or adds to the lighting (lit), a fork this client's own per-batch light mode already performs. A
    /// root that CLEARS 0x08 asks for the CPU fix; that is the stock walker's own behaviour, unmodified.
    void ApplyVertexColor(void* group)
    {
        if (!GetPtr(group, off::kGroupSlots[8].ptrField))
            return; // no MOCV in this group

        void* root = static_cast<off::Group*>(group)->root;
        auto* mohd = root ? static_cast<uint8_t*>(GetPtr(root, off::kOffMohd)) : nullptr;
        const uint32_t mohdFlags = mohd ? Rd32(mohd + off::kOffMohdFlags) : 0;
        if (mohdFlags & off::kMohdFlagSkipColorFix)
        {
            NormalizeVertexColorAlpha(group, static_cast<off::Group*>(group)->flags);
        }
        else
        {
            wxl::game::Native<off::Wmo_FixColorVertexAlphaFn>(off::kFixColorVertexAlpha)(group, nullptr);
            g_vertexColorFixed.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
