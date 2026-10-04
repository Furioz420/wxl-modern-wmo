// Modern-WMO four-layer material: per-vertex height-blend of four diffuse layers, faithful draw path.
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

#include <cstdint>

/**
 * @brief Feature-internal entry points of the four-layer material draw path
 *
 * A modern four-layer material blends four diffuse layers, each sampled at its OWN UV set (layer i =
 * set i, positional), weighted per vertex by a dedicated weight chunk modulated by per-layer height
 * maps, then tinted toward a per-material colour by the weight alpha. The runtime here reproduces that
 * contract on the client's pipeline:
 *   - the loader (WmoNative) serves the four UV sets + weights and the nine texture ids per material;
 *   - at batch bind, the stock single-layer VS/PS pair the client chose is patched in memory (the VS
 *     gains pass-through outputs for the extra UV sets + weights, the PS's single diffuse fetch becomes
 *     the four-layer height blend + tint) and swapped into the GxState shader slots; the extra layer
 *     textures are bound through the engine's own texture states;
 *   - the draw itself is wrapped once (wxl-modern-m2's one-shot interceptor, published as wxl.m2draw) to attach
 *     a second vertex stream carrying the UV sets + weights, with every touched device state restored
 *     right after.
 * Everything the client's own lighting/fog path computes is kept: only the texel that feeds it changes.
 */
namespace wxl::features::wmolayered
{
    /**
     * @brief Attempts the four-layer takeover of the batch bind that just completed.
     *
     * No-op (the batch draws with the stock single-layer effect) unless every prerequisite holds:
     * the current batch's material carries a four-layer snapshot, the group carries the UV/weight side
     * data, all eight layer textures resolve, and the stock VS/PS pair patches cleanly.
     * @param root      map-object root of the batch being drawn.
     * @param group     group being drawn.
     * @param vtxIdx    vertex permutation index the client just bound.
     * @param pixIdx    pixel permutation index the client just bound.
     * @param collection the active effect collection (its slots hold the stock wrappers).
     * @param gxDevice  the engine graphics-device object (GxState owner).
     */
    void TryBindLayered(void* root, void* group, uint32_t vtxIdx, uint32_t pixIdx, void* collection,
                        void* gxDevice);

    /** @brief Clears the extra texture stages a layered batch bound (called at batch-leaf exit). */
    void OnLeafEnd(void* gxDevice);
}

/** @brief Read-only query surface of the four-layer material draw path. */
namespace wxl::runtime::wmolayered
{
    struct Stats
    {
        uint32_t pairsPatched;    ///< distinct stock VS/PS pairs successfully patched
        uint32_t patchFailures;   ///< pairs whose disassembly did not match the expected shape
        uint32_t groupBuffers;    ///< per-group side vertex buffers built
        uint32_t batchesDrawn;    ///< four-layer batches drawn with the patched pair (cumulative)
        uint32_t texturesMissing; ///< binds skipped because a layer texture did not resolve
    };

    Stats GetStats();
}
