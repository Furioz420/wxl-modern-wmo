// Native modern-WMO reader: material resolution -- FileDataID textures + modern-shader family remap.
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
#include "offsets/game/ADT.hpp"
#include "offsets/game/WMO.hpp"

#include <atomic>
#include <cstdint>

namespace off = wxl::offsets::game::wmo;
namespace adt = wxl::offsets::game::adt;

namespace
{
    using namespace wxl::runtime::wmonative::detail;

    using CreateMaterialFn = void(__fastcall*)(void* root, void* edx, int materialIndex);
    CreateMaterialFn g_origCreateMaterial = nullptr;

    /**
     * @brief Modern replacement for the client's material-create routine.
     *
     * Reproduces the stock contract field for field -- idempotent guard on the first handle, the
     * fallback name for an empty texture_1, the shader 3/5/6 -> 4 collapse when texture_2 is empty, and
     * the second texture dropped when the shader pipeline is off -- but sources both names from the
     * FileDataID service instead of a MOTX blob the modern root does not have.
     */
    void __fastcall hkCreateMaterial(void* root, void* edx, int materialIndex)
    {
        if (!RootIsModern(root))
        {
            g_origCreateMaterial(root, edx, materialIndex);
            return;
        }

        auto* r = static_cast<off::Root*>(root);
        auto* materials = static_cast<uint8_t*>(r->materialBase);
        const uint32_t count = r->materialCount;
        if (!materials || materialIndex < 0 || static_cast<uint32_t>(materialIndex) >= count)
            return;

        uint8_t* record = materials + static_cast<size_t>(materialIndex) * off::kMomtStride;
        if (Rd32(record + off::kOffMomtHandle1) != 0)
            return;                                     // already resolved (stock guard)

        auto resolve = [](uint32_t fileDataId) -> const char* {
            if (!fileDataId) return nullptr;
            const char* path = wxl_modern_wmo::ResolveTexture(fileDataId);
            if (path && path[0]) { g_texResolved.fetch_add(1, std::memory_order_relaxed); return path; }
            g_texUnresolved.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        };

        const char* first  = resolve(Rd32(record + off::kOffMomtTexture1));
        const char* second = resolve(Rd32(record + off::kOffMomtTexture2));
        if (!first || !first[0])
            first = off::kFallbackTextureName;
        if (Rd32(reinterpret_cast<void*>(off::kShaderEffectsEnabled)) == 0)
            second = nullptr;

        // Shader id: the client only has effects 0..6, and its shader-effect lookup is UNCHECKED. A
        // modern id selects past the effect table, the render path binds a null effect, and the next
        // shader bind then dereferences near a null address.
        //
        // Remap it onto the nearest native effect by combiner family (NativeShaderFor) rather than
        // flattening everything to 0: the two-layer families keep their second diffuse layer through
        // native effect 6, so the large majority of modern content renders with the blend the file
        // asked for. The env reflection / emissive glow that native 5/6 cannot express is the only loss,
        // and it is the custom-shader follow-up. The rewrite lands on the client's own working buffer,
        // exactly like the 3/5/6 -> 4 rewrite below. Per-family counters make the trade visible in game.
        const uint32_t sourceShader = Rd32(record + off::kOffMomtShader);
        const uint32_t nativeShader =
            g_shaderRemapEnabled.load(std::memory_order_relaxed)
                ? NativeShaderFor(sourceShader)                                   // family remap (default)
                : (sourceShader > off::kMaxClientShaderId ? 0u : sourceShader);   // old single-layer fallback
        if (nativeShader != sourceShader)
        {
            SetU32(record, off::kOffMomtShader, nativeShader);
            g_shaderRemapped.fetch_add(1, std::memory_order_relaxed);
            switch (nativeShader)
            {
                case 6:  g_shaderToTwoLayer.fetch_add(1, std::memory_order_relaxed); break;
                case 5:  g_shaderToEnv.fetch_add(1, std::memory_order_relaxed);      break;
                default: g_shaderToSingle.fetch_add(1, std::memory_order_relaxed);   break;
            }
            const uint32_t bit = 1u << (sourceShader & 31);
            if ((g_shaderSeen.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
                WLOG_INFO("wmo-native: modern shader %u -> native effect %u (%s)", sourceShader,
                          nativeShader,
                          nativeShader == 6 ? "two-layer diffuse"
                                            : nativeShader == 5 ? "env-metal" : "diffuse");
        }

        switch (Rd32(record + off::kOffMomtShader))
        {
            case 3: case 5: case 6:
                if (second && second[0]) break;         // keeps its second texture, shader unchanged
                SetU32(record, off::kOffMomtShader, 4);
                [[fallthrough]];
            case 0: case 1: case 2: case 4:
                second = nullptr;
                break;
            default:
                break;
        }

        auto load = wxl::game::Native<adt::Map_LoadTextureFn>(adt::kMapLoadTexture);
        Wr32(record + off::kOffMomtHandle1,
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(load(first))));
        Wr32(record + off::kOffMomtHandle2,
             second && second[0]
                 ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(load(second)))
                 : 0u);
    }
}

namespace wxl::runtime::wmonative::detail
{
    /**
     * @brief Maps a modern WMO pixel-shader id onto the nearest native 335 effect (0..6).
     *
     * The client ships effects 0..6 (Diffuse, Specular, Metal, Env, Opaque, EnvMetal, TwoLayerDiffuse) and
     * its effect lookup is UNCHECKED, so a modern id selects past the table and the next shader bind
     * faults. The modern enum is a superset built from the SAME families, and across the sampled files
     * every material is blend 0 (opaque), so a family remap keeps far more than the old collapse-to-0 did:
     *   - two-layer families (7, 13, 15/18/19/20, 21) -> 6 TwoLayerDiffuse: the second diffuse layer,
     *     blended by vertex-colour alpha exactly as native 6 does, is preserved. The env/mod2x is dropped.
     *   - env families (11 MaskedEnvMetal, 12 EnvMetalEmissive, 17) -> 5 EnvMetal: base + env kept.
     *   - single-layer families (9 DiffuseEmissive, 10, 14, 16) -> 0 Diffuse.
     * The dropped terms (environment reflection, emissive glow, mod2x) are the only fidelity gap, and
     * they are the custom-shader follow-up. Native ids (<= 6) pass through untouched.
     * @param modern  MOMT shader id straight from the file.
     * @return A shader id in 0..6 the client has an effect for.
     */
    uint32_t NativeShaderFor(uint32_t modern)
    {
        if (modern <= off::kMaxClientShaderId) return modern; // already a native effect
        // Classification taken from the modern shader table: the
        // vertex shader per id decides the layer count and texcoord routing. Map each modern id onto the
        // nearest native effect of the SAME layer family so texture_1 keeps set 0 and a genuine second
        // diffuse layer keeps set 1. Getting the family right matters: id 21 is MapObjLod (SINGLE layer),
        // not two-layer -- routing it through Composite would blend a second texture the shader ignores.
        switch (modern)
        {
            // two-layer diffuse (tex1->set0, tex2->set1): 7 TwoLayerEnvMetal, 9 DiffuseEmissive,
            // 13 TwoLayerDiffuseOpaque, 15 TwoLayerDiffuseEmissive, 18 Mod2x, 19 Mod2xNA, 20 Alpha.
            case 7: case 9: case 13: case 15: case 18: case 19: case 20:
                return 6;
            // env-metal family (tex1->set0 + generated env): 11 MaskedEnvMetal, 12 EnvMetalEmissive,
            // 17 AdditiveMaskedEnvMetal. Their extra set-1 layer is dropped (native 5 cannot express it).
            case 11: case 12: case 17:
                return 5;
            // single-layer (tex1->set0 only): 8 TwoLayerTerrain (2nd coord is vertex-generated, not a MOTV
            // set), 16 Diffuse, 21 MapObjLod, 10/14, and anything unforeseen.
            default:
                return 0;
        }
    }

    bool InstallMaterial()
    {
        return wxl_modern_wmo::HookAttachByName("Wmo.CreateMaterial", &hkCreateMaterial, &g_origCreateMaterial);
    }
}
