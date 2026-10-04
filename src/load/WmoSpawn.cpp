// Map-object spawn / root-complete detours: apply the per-instance MODF scale, publish OnWmoRootLoad.
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

#include "../ExtensionApi.hpp"
#include "WmoNativeShared.hpp"

#include "offsets/game/WMO.hpp"

#include <cstdint>

namespace
{
    namespace wmo = wxl::offsets::game::wmo;

    wmo::Wmo_SpawnFromModfFn g_origWmoSpawn = nullptr;
    wmo::Wmo_RootCompleteFn  g_origWmoRoot  = nullptr;

    /** @brief Multiplies the upper-left 3x3 rows of a 4x4 row-major matrix by a uniform factor. */
    void ScaleMatrixRows3x3(float* m, float s)
    {
        m[0] *= s; m[1] *= s; m[2]  *= s;
        m[4] *= s; m[5] *= s; m[6]  *= s;
        m[8] *= s; m[9] *= s; m[10] *= s;
    }

    /**
     * @brief Reports a freshly spawned WMO's live doodad-set selection against its loaded MODS.
     *
     * Catches the in-game "all doodad sets render at once" case by reading the post-down-convert MODS the
     * Client actually loaded (not the on-disk file) plus the instance's selected/extra sets. Logs only a
     * suspicious shape: an extra set populated, a selected index out of range, or a set 0 whose MODD range
     * swallows the other content sets (every doodad then resolves to set 0, which renders unconditionally).
     * A correctly selected WMO stays silent.
     * @param inst  freshly spawned WMO instance.
     */
    void DiagDoodadSets(void* instRaw)
    {
        auto* inst = static_cast<wmo::Instance*>(instRaw);
        auto* root = reinterpret_cast<uint8_t*>(inst->root);
        if (!root)
            return;
        const uint32_t nSets = *reinterpret_cast<uint32_t*>(root + wmo::kOffRootDoodadSets);
        if (nSets < 2)
            return; // a single-set WMO cannot show "extra" sets
        const uint8_t* mods = *reinterpret_cast<uint8_t**>(root + wmo::kOffRootMods);
        if (!mods)
            return;
        const uint32_t nDefs = *reinterpret_cast<uint32_t*>(root + wmo::kOffRootDoodadDefs);
        const uint32_t sel    = inst->doodadSet;
        const uint16_t* extra = inst->extraSets;
        const char* name = reinterpret_cast<const wmo::Root*>(root)->nameInline;
        const uint32_t s0count = *reinterpret_cast<const uint32_t*>(mods + wmo::kOffModsCount);

        const bool greedy0  = nDefs && s0count + 1 >= nDefs;    // set 0 covers (almost) every def
        const bool selOob   = sel >= nSets;                     // selected index out of range
        const bool hasExtra = extra[0] || extra[1] || extra[2]; // extra sets populated
        if (!greedy0 && !selOob && !hasExtra)
            return; // selection resolves to {set0, sel} only -> correct, stay silent

        WLOG_INFO("wmo-doodad-diag: %.96s nSets=%u nDefs=%u sel=%u extra={%u,%u,%u} set0count=%u%s%s%s",
            name, nSets, nDefs, sel, extra[0], extra[1], extra[2], s0count,
            greedy0 ? " GREEDY-SET0" : "", selOob ? " SEL-OOB" : "", hasExtra ? " EXTRA-SETS" : "");
    }

    /**
     * @brief Detours the WMO instance spawn, applying the per-instance MODF scale the Client ignores.
     *
     * The Client builds the instance at scale 1.0 (MODF+0x3E is padding to it). After the native spawn,
     * the modern scale is folded into the render matrix (+0x70).
     *
     * The collision/portal copy (+0xB0) is the TRANSPOSED basis, which the portal-visibility test reads
     * as an inverse rotation. Leaving it at 1.0 means the geometry is drawn at scale s while every
     * culling volume, portal plane and bound still describes the WMO at its native size -- on a map
     * where 113 distinct scale factors are in use, down to 0.5x, that mismatch IS the "portal / frustum"
     * symptom. Scaling it by s (tried before) makes it worse and hides interiors, and the reason is
     * arithmetic: for an orthonormal R, transpose(R) == inverse(R), and inverse(R * s) == inverse(R) / s.
     * The transposed copy must therefore be scaled by 1/s, not s. Kept behind a switch because the
     * earlier attempt in the wrong direction was found empirically to make WMOs vanish.
     *
     * A dedup hit returns an already-scaled instance, so the scale is applied only to a freshly built
     * instance, recognised by its still-orthonormal basis (|row0| == 1); this is reload-safe and needs
     * no per-instance bookkeeping.
     * @param ctx         world context.
     * @param modf        MODF placement record.
     * @param tileOrigin  tile world origin.
     * @param dedup       non-zero to return an existing instance for a known uniqueId.
     * @return the spawned (or existing) instance.
     */
    void* __cdecl hkWmoSpawn(void* ctx, void* modf, const float* tileOrigin, int dedup)
    {
        void* inst = g_origWmoSpawn(ctx, modf, tileOrigin, dedup);
        if (!inst || !modf)
            return inst;

        DiagDoodadSets(inst);

        const uint16_t raw = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(modf) + wmo::kOffModfScale);
        if (raw == 0 || raw == 1024)
            return inst; // native / unscaled: leave the instance byte-for-byte
        const float s = static_cast<float>(raw) / 1024.0f;

        auto* instance = static_cast<wmo::Instance*>(inst);
        float* render = instance->renderMatrix;
        const float len2 = render[0] * render[0] + render[1] * render[1] + render[2] * render[2];
        if (len2 < 0.9999f || len2 > 1.0001f)
            return inst; // already scaled (a dedup hit returned an existing instance)

        ScaleMatrixRows3x3(render, s);

        // The transposed collision/portal basis is an INVERSE rotation, so it takes 1/s (see above).
        if constexpr (wxl_modern_wmo::kEnabled)
            ScaleMatrixRows3x3(instance->collisionMatrix, 1.0f / s);
        return inst;
    }

    /**
     * @brief Detours WMO root read-completion: hands off to WmoAsync's SubmitRootComplete, which owns
     *        both the OnWmoRootLoad emission and the classify/background-submit decision.
     *
     * OnWmoRootLoad fires once after the async read fills the root buffer and before the walker runs, so
     * a subscriber may reshape the root buffer in place; the native walk then reads the reshaped bytes.
     * The emission itself lives in SubmitRootComplete now, not here -- it must run AFTER that function
     * arms this root's background-walk gate (if it decides to background it) and before anything else,
     * so a reentrant WaitLoad/WaitLoadGroup call during event dispatch resolves through wxl's own gate
     * instead of falling through to a native wait unsafe to call this early (see SubmitRootComplete's own
     * doc comment in WmoAsync.cpp).
     * @param root  map-object root whose buffer was just read.
     */
    void __cdecl hkWmoRootComplete(void* root)
    {
        wxl::runtime::wmonative::detail::SubmitRootComplete(root, g_origWmoRoot);
    }
}

namespace wxl_modern_wmo
{
    bool InstallWmoSpawn()
    {
        HookAttachByName("Wmo.SpawnFromModf", &hkWmoSpawn, &g_origWmoSpawn);
        HookAttachByName("Wmo.RootComplete", &hkWmoRootComplete, &g_origWmoRoot);
        return true;
    }
}
