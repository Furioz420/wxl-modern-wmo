// Modern-WMO Composite fix: composite the second layer by the texture's own alpha, in-memory PS patch.
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
//
// MECHANISM
//   The stock two-layer WMO pixel shader (effect id 6) does, per pixel:
//       texld r0, t0, s0            ; r0 = base   (texture_1)
//       texld r1, t1, s1            ; r1 = overlay(texture_2)
//       lrp   r2, v1.w, r0, r1      ; r2 = lerp(r1, r0, v1.w)  -- blend by SECONDARY vertex-colour alpha
//   A modern material's texture_2 is an alpha-masked detail overlay (measured 62-92% transparent), so
//   this blend mixes in the overlay's dark body and renders its highlight lines as dark stripes.
//   The fix rewrites that ONE instruction to composite by the overlay's own alpha:
//       lrp r2, r1.w, r1, r0        ; r2 = lerp(base, overlay, overlay.a)
//   at r1.w = 0 (transparent) the base shows; at r1.w = 1 (a highlight texel) the overlay shows. The
//   patch is applied IN MEMORY to a copy of the stock shader (disassemble -> rewrite -> reassemble); no
//   .bls file is produced. It is scoped to MODERN WMOs -- stock two-layer content still wants the
//   vertex-alpha blend -- via two frame-stash hooks on the WMO batch-draw leaves plus a post-hook on the
//   effect bind that swaps GxState slot 0x4E only when the current root is modern and the active effect
//   collection is the Composite one.
//
//   The same effect-bind post-hook is the entry of the four-layer material path (LayeredShader): a
//   modern single-layer bind whose batch material carries a four-layer snapshot is handed over there.

#include "CompositeShader.hpp"
#include "../ExtensionApi.hpp"
#include "LayeredShader.hpp"
#include "ShaderPatch.hpp"
#include "../load/WmoNative.hpp"

#include "game/Gx.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Shader.hpp"
#include "offsets/game/WMO.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
    namespace shoff = wxl::offsets::engine::shader;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace wmo   = wxl::offsets::game::wmo;
    namespace sp    = wxl::features::wmoshader;

    template <class T>
    inline T& At(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

    // GxState setter, the same address/convention the shader-slot swaps use.
    using GxSetFn = void(__fastcall*)(void* device, void* edx, uint32_t selector, void* value);

    // ------------------------------------------------------------------ state
    std::atomic<bool>     g_fixEnabled{ true };
    bool                  g_installed = false;

    wmo::Wmo_RenderLeafFn g_origExtRender = nullptr;
    wmo::Wmo_RenderLeafFn g_origIntRender = nullptr;
    shoff::EffectBindFn   g_origEffectBind = nullptr;

    // Frame stash for the duration of a MODERN WMO batch-draw leaf; read in the effect-bind post-hook.
    // Render is single-threaded (the client has no MT render), so plain values are sufficient.
    bool  g_curModern = false;
    void* g_curRoot   = nullptr;
    void* g_curGroup  = nullptr;

    // Stock Composite PS wrapper -> patched wrapper (null = draw stock).
    std::unordered_map<void*, void*> g_patched;

    std::atomic<uint32_t> g_statPatched{ 0 }, g_statPatchFail{ 0 }, g_statBatches{ 0 };

    // ------------------------------------------------------------------ disassembly surgery
    /// One parsed `texld rDest, coord, s<sampler>` statement.
    struct Texld { std::string dest, coord; bool found = false; };

    /// Finds the first `texld rDest, coord, s<sampler>` and returns its dest + coord operands.
    Texld FindTexld(const std::string& text, unsigned sampler)
    {
        Texld out;
        char sTok[8];
        std::snprintf(sTok, sizeof sTok, "s%u", sampler);
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
        };
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            const std::string line = text.substr(pos, eol - pos);
            const size_t t = line.find("texld");
            if (t != std::string::npos)
            {
                const size_t c1 = line.find(',', t);
                const size_t c2 = c1 == std::string::npos ? std::string::npos : line.find(',', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos && trim(line.substr(c2 + 1)) == sTok)
                {
                    out.dest  = trim(line.substr(t + 5, c1 - (t + 5)));
                    out.coord = trim(line.substr(c1 + 1, c2 - (c1 + 1)));
                    out.found = !out.dest.empty() && !out.coord.empty();
                    return out;
                }
            }
            pos = eol + 1;
        }
        return out;
    }

    /**
     * @brief Rewrites the Composite pixel shader to composite the second layer by the OVERLAY texture's
     *        own alpha: the stock operand triplet `v1.w, <base>, <overlay>` (lrp factor = secondary
     *        vertex alpha, then the two texld results) becomes `<overlay>.w, <overlay>, <base>` --
     *        flipping the lerp direction and sourcing the factor from the overlay's alpha.
     * Returns "" if the shape does not match (any non-Composite permutation is drawn stock).
     */
    std::string InjectComposite(const std::string& text)
    {
        const Texld base    = FindTexld(text, 0);
        const Texld overlay = FindTexld(text, 1);
        if (!base.found || !overlay.found) return {};

        const std::string stock = "v1.w, " + base.dest + ", " + overlay.dest;
        const size_t at = text.find(stock);
        if (at == std::string::npos) return {};
        size_t lineStart = text.rfind('\n', at);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        if (text.find("lrp", lineStart) == std::string::npos || text.find("lrp", lineStart) > at)
            return {};

        std::string out = text;
        out.replace(at, stock.size(), overlay.dest + ".w, " + overlay.dest + ", " + base.dest);
        return out;
    }

    /// Builds the Composite replacement for one stock wrapper (null = fail / not Composite).
    void* BuildPatched(void* stock)
    {
        const uint8_t* code = At<const uint8_t*>(stock, shoff::kCgxShaderBytePtr);
        const uint32_t len  = At<uint32_t>(stock, shoff::kCgxShaderByteLen);
        if (!code || len < 8 || len > 0x20000) return nullptr;
        uint32_t ver = 0;
        std::memcpy(&ver, code, 4);
        if (ver != 0xFFFF0200u && ver != 0xFFFF0300u) return nullptr; // ps_2_0 / ps_3_0 only

        const std::string text = sp::Disassemble(code, len);
        if (text.empty()) return nullptr;
        const std::string patched = InjectComposite(text);
        if (patched.empty()) return nullptr; // not the two-texture Composite shape -> draw stock

        ID3DBlob* blob = sp::Assemble(patched, "wxlComposite");
        if (!blob) return nullptr;
        void* wrapper = sp::MakeWrapper(blob->GetBufferPointer(),
                                        static_cast<uint32_t>(blob->GetBufferSize()), false);
        const uint32_t outLen = static_cast<uint32_t>(blob->GetBufferSize());
        blob->Release();
        if (!wrapper) return nullptr;

        WLOG_INFO("wmo-composite: patched Composite PS (%u -> %u B)", len, outLen);
        return wrapper;
    }

    /// Cached build per stock permutation; null means "draw stock".
    void* GetPatched(void* stock)
    {
        auto it = g_patched.find(stock);
        if (it != g_patched.end()) return it->second;
        void* built = BuildPatched(stock);
        g_patched.emplace(stock, built);
        if (built) g_statPatched.fetch_add(1, std::memory_order_relaxed);
        else       g_statPatchFail.fetch_add(1, std::memory_order_relaxed);
        return built;
    }

    /// True when `col` is a single-layer WMO effect collection (Diffuse 0 / Opaque 4 / EnvMetal 5) in
    /// either the AltRender table (modern path) or the exterior table.
    bool IsSingleLayerCollection(void* col)
    {
        for (unsigned i : { 0u, 4u, 5u })
        {
            if (col == *reinterpret_cast<void**>(shoff::kAltEffectTable + i * 4) ||
                col == *reinterpret_cast<void**>(shoff::kExteriorEffectTable + i * 4))
                return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ the detours
    /// Frame-stash: record the root/group whose batches this leaf is about to draw and whether the root
    /// is modern. The two entry leaves bracket the delegated (MOHD flag 0x2) path too, so this covers
    /// all modern rendering. The layered path's per-leaf cleanup runs on exit.
    void __fastcall hkExtRender(void* root, void* edx, void* group, int flag)
    {
        const bool prevM = g_curModern; void* prevR = g_curRoot; void* prevG = g_curGroup;
        g_curModern = wxl::runtime::wmonative::IsModernRoot(root);
        g_curRoot  = root;
        g_curGroup = group;
        g_origExtRender(root, edx, group, flag);
        wxl::features::wmolayered::OnLeafEnd(*reinterpret_cast<void**>(gxoff::kGxDevicePtr));
        g_curModern = prevM; g_curRoot = prevR; g_curGroup = prevG;
    }

    void __fastcall hkIntRender(void* root, void* edx, void* group, int flag)
    {
        const bool prevM = g_curModern; void* prevR = g_curRoot; void* prevG = g_curGroup;
        g_curModern = wxl::runtime::wmonative::IsModernRoot(root);
        g_curRoot  = root;
        g_curGroup = group;
        g_origIntRender(root, edx, group, flag);
        wxl::features::wmolayered::OnLeafEnd(*reinterpret_cast<void**>(gxoff::kGxDevicePtr));
        g_curModern = prevM; g_curRoot = prevR; g_curGroup = prevG;
    }

    /// Effect-bind post-hook. After the stock VS/PS wrappers are in GxState (0x4D vertex, 0x4E pixel),
    /// override them for a modern WMO batch. TWO routes, one per effect family:
    ///  - Composite (shader 6, two-layer): swap the PIXEL shader (0x4E) for the alpha-composite copy.
    ///  - single-layer collections: hand over to the four-layer path, which takes the batch only when
    ///    its material carries a four-layer snapshot (and is a no-op otherwise).
    /// The immediately following indexed draw flushes the slots; the next batch's bind restores stock.
    void __cdecl hkEffectBind(uint32_t vtxIdx, uint32_t pixIdx)
    {
        g_origEffectBind(vtxIdx, pixIdx);
        if (!g_curModern) return;

        void* col = *reinterpret_cast<void**>(shoff::kActiveCollection);
        if (!col) return;
        void* gxDev = *reinterpret_cast<void**>(gxoff::kGxDevicePtr);
        if (!gxDev) return;

        // Two-layer Composite: pixel-shader swap. Modern WMOs delegate to AltRender, which binds from the
        // 0xD1C3D4 table, so their Composite collection is *kAltEffectComposite; the non-delegated exterior
        // path uses *kExteriorEffectComposite. Accept either.
        if (col == *reinterpret_cast<void**>(shoff::kAltEffectComposite) ||
            col == *reinterpret_cast<void**>(shoff::kExteriorEffectComposite))
        {
            if (!g_fixEnabled.load(std::memory_order_relaxed)) return; // composite A/B only; layered stays on
            void* stock = *reinterpret_cast<void**>(static_cast<uint8_t*>(col) +
                                                    pixIdx * 4u + shoff::kCollectionPixSlots);
            if (!stock) return;
            void* patched = GetPatched(stock);
            if (!patched) return;
            reinterpret_cast<GxSetFn>(shoff::kGxStateSet)(gxDev, nullptr, shoff::kStatePixelShader, patched);
            g_statBatches.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Four-layer material on a single-layer bind (its shader id remaps to Diffuse): the layered
        // path decides from the batch's material snapshot and binds its own patched pair + textures.
        if (IsSingleLayerCollection(col))
            wxl::features::wmolayered::TryBindLayered(g_curRoot, g_curGroup, vtxIdx, pixIdx, col, gxDev);
    }
}

// ---------------------------------------------------------------- public query surface
namespace wxl::runtime::wmocomposite
{
    Stats GetStats()
    {
        Stats s{};
        s.patchedShaders = g_statPatched.load(std::memory_order_relaxed);
        s.patchFailures  = g_statPatchFail.load(std::memory_order_relaxed);
        s.batchesSwapped = g_statBatches.load(std::memory_order_relaxed);
        return s;
    }

    bool Enabled() { return wxl_modern_wmo::kEnabled; }
    bool Installed() { return g_installed; }

    bool FixEnabled() { return g_fixEnabled.load(std::memory_order_relaxed); }
    void SetFixEnabled(bool on) { g_fixEnabled.store(on, std::memory_order_relaxed); }
}

namespace wxl_modern_wmo
{
    bool InstallCompositeShader()
    {
        const bool ext  = HookAttachByName("Wmo.ExtRender", &hkExtRender, &g_origExtRender);
        const bool intr = HookAttachByName("Wmo.IntRender", &hkIntRender, &g_origIntRender);
        const bool bind = HookAttachByName("Wmo.CompositeEffectBind", &hkEffectBind, &g_origEffectBind);
        g_installed = ext && intr && bind;
        if (!g_installed)
            WLOG_WARN("wmo-composite: install failed (ext=%d int=%d bind=%d); Composite renders stock",
                      ext ? 1 : 0, intr ? 1 : 0, bind ? 1 : 0);
        else
            WLOG_INFO("wmo-composite: second-layer alpha-composite fix + four-layer path installed");
        return true; // non-fatal
    }
}
