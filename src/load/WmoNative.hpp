// Native modern-WMO reader: tag-driven chunk walkers replacing the client's positional ones.
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
 * @brief Read-only query surface of the native modern-WMO reader
 *
 * The client's root and group chunk walkers are POSITIONAL: they skip MVER blind, then consume a fixed
 * sequence of chunks, storing a content pointer and a count derived from `chunkSize / stride`, without
 * ever comparing a FourCC (the sole exception is the optional trailing MCVP). They also carry no bounds
 * check, no validation and no early return, so a modern root -- which drops MOTX/MODN/MOSB/MOVV/MOVB and
 * inserts GFID/MODI/MGI2/MDDI/MNLD/MFED/MAVG -- desynchronises every pointer that follows, silently.
 *
 * This feature replaces the three walkers with tag-driven ones that write exactly the same runtime
 * fields, so a modern WMO is read WHERE IT LIES: no chunk is reordered, no buffer is rebuilt, no
 * client-shaped file is synthesized. A stock WMO is classified in one cheap pass and handed to the
 * untouched original walker, so mixed scenes stay correct.
 *
 * Faithfulness note: the stock loader itself writes into the loaded file buffer (MOGI sky-flag clear,
 * MOPT NaN plane repair, MOCV alpha rewrite, MOMT texture handles). Those are the client's own
 * semantics on its own working memory, and the replacement walkers reproduce them exactly.
 */
namespace wxl::runtime::wmonative
{
    /** @brief Session counters of the native WMO reader (all monotonic). */
    struct Stats
    {
        uint32_t rootsModern;        ///< roots classified modern and walked by the tag-driven walker
        uint32_t rootsStock;         ///< roots handed to the untouched original walker
        uint32_t groupsModern;       ///< groups walked by the tag-driven walker
        uint32_t groupsFailed;       ///< groups whose walk faulted (SEH) -- left empty, never a crash
        uint32_t texturesResolved;   ///< MOMT texture FileDataIDs resolved to a client path
        uint32_t texturesUnresolved; ///< MOMT texture FileDataIDs the DB2 authority could not resolve
        uint32_t parkedDoodadDefs;   ///< roots whose MODD was parked (MODI index form, no MODN blob)
        uint32_t parkedLiquids;      ///< groups whose MLIQ was parked (modern liquid ids crash the client)
        uint32_t parkedIndex32;      ///< groups carrying MOVX (32-bit indices) with no 335 home
        uint32_t unknownChunks;      ///< modern-only chunks seen and skipped (GFID, MGI2, MNLD, ...)
        /// Materials whose modern shader id was remapped onto the nearest native effect (0..6) by
        /// combiner family, split by target below. The env reflection / emissive glow that native 5/6
        /// cannot express is the remaining fidelity gap and the custom-shader follow-up.
        uint32_t shaderRemapped;     ///< total materials remapped (sum of the three below)
        uint32_t shaderToTwoLayer;   ///< -> native 6 TwoLayerDiffuse (second diffuse layer kept)
        uint32_t shaderToEnv;        ///< -> native 5 EnvMetal (base + env kept, emissive dropped)
        uint32_t shaderToSingle;     ///< -> native 0 Diffuse (single layer)
        /// Collision-only groups (geometry, no MOTV/MONR, no batches) whose runtime vertex count was
        /// zeroed so the stock vertex fill -- which reads those arrays unconditionally -- skips them.
        uint32_t parkedNoUvGroups;
        /// MOBA batches whose modern u16 material index was copied into the byte this client reads.
        /// Without it every batch draws material 0 -- one texture for a whole building.
        uint32_t materialIdsMoved;
        uint32_t materialIdsOutOfRange; ///< indices above 255, unreachable by the client's `movzx byte`
        /// Per-batch AABB culls bypassed because the batch is modern and ships no box (cumulative, per
        /// frame). Group-level frustum / portal / horizon culling still applies.
        uint32_t batchCullBypassed;
        /// Modern groups whose MOCV took the stock CPU vertex-colour fix because their root clears the
        /// MOHD do_not_fix flag (the legacy contract, honoured exactly as the stock walker does). Groups
        /// under a root that SETS the flag -- every modern file observed -- instead get the modern alpha-only
        /// rewrite (opaque exterior / zero interior past the trans range, RGB shipped raw), unconditionally.
        uint32_t vertexColorFixed;
        uint32_t uvTransformed; ///< groups whose UV set was transformed by the experimental UvMode probe
        /// Four-layer (shader 23) groundwork gathered at load: groups carrying the per-vertex layer
        /// weights chunk with all four UV sets, and materials whose nine texture ids were snapshotted
        /// before the client's material creation overwrites two of them.
        uint32_t layeredGroups;
        uint32_t layeredMaterials;
    };

    /** @brief Returns a snapshot of the session counters. */
    Stats GetStats();

    /** @brief True when the native reader is compiled in (wxl_modern_wmo::kEnabled). */
    bool Enabled();

    /** @brief True when every walker detour installed; false leaves the client fully stock. */
    bool Installed();

    /**
     * @brief Reports whether a loaded root was classified as a modern container.
     * @param root  map-object root object.
     * @return true when its buffer carries no MOTX or carries GFID.
     */
    bool IsModernRoot(void* root);

    /** @brief Live A/B of the modern-shader family remap. False reverts to the single-layer fallback. */
    bool ShaderRemapEnabled();
    void SetShaderRemapEnabled(bool on);

    /**
     * @brief EXPERIMENTAL UV-orientation probe. 0 = untouched; 1 swap u/v; 2 rot+90; 3 rot-90; 4 flip v;
     *        5 flip u; 6 feed the second UV set to the base texture. Applied at group load, so a change
     *        takes effect on the next (re)load of a WMO -- walk away and back. Finds the transform that
     *        de-rotates modern WMO textures empirically before it is made permanent.
     */
    int  UvMode();
    void SetUvMode(int mode);

    // --- four-layer (shader 23) material groundwork -------------------------------------------------
    // A modern four-layer material carries nine texture FileDataIDs in its record and per-vertex layer
    // weights in a dedicated group chunk (4 bytes per vertex, alongside four UV sets). The reader
    // snapshots both at load -- the ids at root walk, BEFORE the client's material creation overwrites
    // the two that alias its runtime handle fields -- and serves them to the layered draw path.

    /** @brief One four-layer material: the nine texture ids, the tint colour, and lazily resolved
     *  texture handles. Handles are filled by the draw path on the render thread (texturesTried gates
     *  the one resolve attempt); the loader only writes the ids. */
    struct LayeredMaterial
    {
        uint32_t diffuseFdid[4]; ///< layer A..D diffuse texture ids
        uint32_t heightFdid[4];  ///< layer A..D height-map ids (height in the ALPHA channel)
        uint32_t envFdid;        ///< environment-map id (specular term; unused until the env pass lands)
        uint32_t tintBgra;       ///< per-material tint colour (record diffuse colour, BGRA)
        uint32_t flags;          ///< material flags (clamp bits applied to every layer's wrap mode)
        void*    diffuseTex[4];  ///< resolved texture handles (draw-path cache, render thread only)
        void*    heightTex[4];
        bool     texturesTried;  ///< one resolve attempt made; missing handles stay null (stock draw)
    };

    /**
     * @brief Looks up the four-layer snapshot of one material.
     * @param root           map-object root object.
     * @param materialIndex  MOMT record index (from the batch record).
     * @return the snapshot, or null when the material is not a four-layer one. The pointer stays valid
     *         until the same root is walked again (root reload); consume it within the current draw.
     */
    LayeredMaterial* FindLayeredMaterial(void* root, uint32_t materialIndex);

    /** @brief Per-group vertex side data of the layered path: the four UV set pointers and the
     *  per-vertex layer-weight chunk, all pointing into the group's resident file buffer. */
    struct LayeredGroup
    {
        const void* motv[4];   ///< UV sets in file order (layer i samples set i, 1:1)
        const void* weights;   ///< 4 bytes per vertex: layer A/B/C weights + tint factor (BGRA)
        uint32_t    vertexCount;
    };

    /**
     * @brief Fetches the layered side data of a group (present only when the group shipped all four UV
     *        sets and the weight chunk). Same lifetime contract as FindLayeredMaterial.
     */
    bool GetLayeredGroup(void* group, LayeredGroup& out);

    /** @brief Material index of one batch record (modern u16 form or the stock byte). */
    uint32_t BatchMaterialIndex(const void* mobaRecord);

    /**
     * @brief The batch record most recently kept by the per-batch cull on a modern leaf (the one the
     *        immediately following effect bind belongs to). Null before any modern batch, or once
     *        consumed -- see ClearCurrentBatch(). Render thread only.
     */
    const void* CurrentBatch();

    /**
     * @brief Consumes the CurrentBatch() latch (sets it back to null).
     *
     * MUST be called by whatever effect-bind reads CurrentBatch(), immediately after reading it,
     * whether or not the value turned out to be usable. The cull hook that sets it (hkCullBatch) has no
     * way to know which later effect-bind call, if any, is really "the one that follows" -- an ordinary
     * single-layer bind for an unrelated batch (or root, even a since-unloaded one) can occur first, and
     * would otherwise silently act on a stale, potentially dangling pointer instead of finding no batch
     * armed. Confirmed off a real crash dump: an un-consumed latch surviving across a WMO group
     * unload during a load burst (e.g. a zone reconnect) reads freed memory.
     */
    void ClearCurrentBatch();
}
