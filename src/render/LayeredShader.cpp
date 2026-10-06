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

// MECHANISM
//   The four-layer material's ground truth (read from the modern uber shader's dedicated branch):
//     wD    = 1 - sat(wA + wB + wC)                      per-vertex weights, D is the remainder
//     h_i   = max(heightTex_i.a, 0.004)                  height maps, height in the ALPHA channel
//     k_i   = w_i * h_i
//     m     = max_i k_i
//     b_i   = (1 - sat(m - k_i)) * k_i                   peak-relative height blend
//     w'_i  = b_i / sum(b)                               renormalised
//     blend = sum_i diffuse_i(uv set i) * w'_i           layer i samples UV set i, 1:1, untransformed
//     rgb   = lerp(blend.rgb, tint.rgb, w.w)             per-material tint by the weight alpha
//   and the result then feeds the standard combine (texture x vertex colour x lighting x 2) that the
//   client's pipeline already performs. So the takeover is surgical: the stock single-layer VS gains four
//   pass-through outputs (UV sets B..D + weights, sourced from a second vertex stream), the stock PS's
//   single diffuse fetch becomes the block above, and NOTHING else changes -- the client's own
//   lighting, fog and alpha path run untouched on the blended texel.
//
//   Data path per batch: the loader side data (four MOTV sets + the weight chunk) is copied once per
//   group into a MANAGED second vertex stream (36 B/vertex); the draw is wrapped by wxl-modern-m2's one-shot
//   interceptor (wxl.m2draw, since wxl-modern-m2 owns the DrawIndexedPrimitive vtable slot), which attaches
//   stream 1 + a custom declaration, rebases stream 0 so both streams share the group's vertex indexing,
//   issues the native draw, and restores every touched device state. Layer textures ride the engine's
//   own texture states (flushed with the draw like the client's own binds), so no raw SetTexture ever
//   races the state cache.
//
//   Not reproduced yet (reported honestly): the environment/specular add-on term (needs a reflection
//   interpolant the stock single-layer VS does not compute) and pre-SM3 shader profiles (the patchers
//   require the vs_3_0/ps_3_0 forms; older profiles keep the stock single-layer look, counted).

#include "../ExtensionApi.hpp"
#include "LayeredShader.hpp"
#include "ShaderPatch.hpp"
#include "../load/WmoNative.hpp"

#include "game/Binding.hpp"
#include "game/Gx.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Shader.hpp"
#include "offsets/game/ADT.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/WMO.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cctype>
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
    namespace m2off = wxl::offsets::game::m2;
    namespace adt   = wxl::offsets::game::adt;
    namespace rt    = wxl::runtime::wmonative;
    namespace sp    = wxl::features::wmoshader;

    using GxSetFn = void(__fastcall*)(void* device, void* edx, uint32_t selector, void* value);

    // ------------------------------------------------------------------ state
    /// Second vertex stream layout: four UV sets (float2 each) + the weight dword. The weight bytes are
    /// copied raw; the declaration reads them as D3DCOLOR, whose byte order makes stored byte 2 the
    /// shader's .x -- the layer-A slot, matching the one-hot population measured across the sampled files.
    constexpr uint32_t kStream1Stride = 4 * 8 + 4;

    struct PatchedPair
    {
        void* vs = nullptr;   ///< patched GPU-shader wrapper for GxState slot 0x4D
        void* ps = nullptr;   ///< patched GPU-shader wrapper for GxState slot 0x4E
        int   tintReg = -1;   ///< PS constant register receiving {tint.rgb, enable}
    };

    struct GroupBuffer
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        const void* srcMotv0  = nullptr;   // identity of the side data the buffer was built from
        const void* srcWeights = nullptr;
        uint32_t    verts = 0;
    };

    // Caches live for the session (render thread only). A device RE-CREATE (not Reset: the stream
    // buffer is MANAGED, declarations and shaders survive a Reset) would strand them -- the same
    // exposure the existing composite patch has.
    std::unordered_map<uint64_t, PatchedPair> g_pairs;   // key = stock VS/PS wrapper pair
    std::unordered_map<void*, GroupBuffer>    g_groupVb; // key = group runtime object
    IDirect3DVertexDeclaration9*              g_decl = nullptr;

    /// Armed between a successful bind and its draw; consumed by the interceptor.
    IDirect3DVertexBuffer9* g_pendingVb = nullptr;
    bool g_stagesBound = false;   ///< extra texture stages carry layer textures (cleared at leaf end)

    std::atomic<uint32_t> g_statPairs{0}, g_statPatchFail{0}, g_statGroupVb{0};
    std::atomic<uint32_t> g_statBatches{0}, g_statTexMissing{0};
    uint32_t g_versionLogged = 0;   ///< one log line per unexpected shader profile

    // ------------------------------------------------------------------ disassembly text utilities
    struct DclInfo
    {
        std::string usage;   // "position", "normal", "color", "texcoord", "2d", ...
        int         index = 0;
        char        regClass = 0;  // 'v', 'o', 's'
        int         reg = -1;
    };

    /// Parses one "dcl_<usage><idx> <reg>[.mask]" line; false when the line is not a declaration.
    bool ParseDcl(const std::string& line, DclInfo& out)
    {
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line.compare(p, 4, "dcl_") != 0) return false;
        size_t u = p + 4;
        size_t ue = u;
        while (ue < line.size() && (std::isalpha(static_cast<unsigned char>(line[ue])) )) ++ue;
        out.usage.assign(line, u, ue - u);
        out.index = 0;
        while (ue < line.size() && std::isdigit(static_cast<unsigned char>(line[ue])))
            out.index = out.index * 10 + (line[ue++] - '0');
        size_t r = line.find_first_not_of(" \t", ue);
        if (r == std::string::npos) return false;
        out.regClass = line[r];
        if (out.regClass != 'v' && out.regClass != 'o' && out.regClass != 's') return false;
        size_t re = r + 1;
        out.reg = 0;
        bool any = false;
        while (re < line.size() && std::isdigit(static_cast<unsigned char>(line[re])))
        {
            out.reg = out.reg * 10 + (line[re++] - '0');
            any = true;
        }
        return any;
    }

    /// Highest register number of one class ('r', 'v', 'c', 'o') used anywhere in the text.
    int MaxRegister(const std::string& text, char cls)
    {
        int maxReg = -1;
        for (size_t p = 0; (p = text.find(cls, p)) != std::string::npos; ++p)
        {
            if (p && (std::isalnum(static_cast<unsigned char>(text[p - 1])) || text[p - 1] == '_'))
                continue;
            size_t q = p + 1;
            int v = 0;
            bool any = false;
            while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q])))
            {
                v = v * 10 + (text[q++] - '0');
                any = true;
            }
            if (any && v > maxReg) maxReg = v;
        }
        return maxReg;
    }

    /// Byte offset just past the LAST declaration line (the canonical insertion point for new dcls).
    size_t AfterLastDcl(const std::string& text)
    {
        size_t last = std::string::npos;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            DclInfo d;
            if (ParseDcl(text.substr(pos, eol - pos), d))
                last = eol + 1;
            pos = eol + 1;
        }
        return last;
    }

    /// Finds the first `texld <dest>, <coord>, s<sampler>` line; all operands returned as written.
    struct TexldLine { std::string dest, coord; size_t at = std::string::npos, len = 0; };
    TexldLine FindTexld(const std::string& text, unsigned sampler)
    {
        TexldLine out;
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
            const size_t t = line.find("texld ");
            if (t != std::string::npos)
            {
                const size_t c1 = line.find(',', t);
                const size_t c2 = c1 == std::string::npos ? std::string::npos : line.find(',', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos && trim(line.substr(c2 + 1)) == sTok)
                {
                    out.dest  = trim(line.substr(t + 6, c1 - (t + 6)));
                    out.coord = trim(line.substr(c1 + 1, c2 - (c1 + 1)));
                    out.at    = pos;
                    out.len   = eol - pos;
                    return out;
                }
            }
            pos = eol + 1;
        }
        return out;
    }

    // ------------------------------------------------------------------ the vertex-shader patch
    /// Output usage indices chosen for the extra interpolants (shared VS -> PS contract per pair).
    struct VsMapping { int uv[3]; int weights; };

    /**
     * @brief Patches a stock single-layer vs_3_0: adds inputs for UV sets B..D (stream-1 TEXCOORD1..3)
     *        and the weights (stream-1 COLOR1), plus pass-through outputs on free texcoord usages.
     *
     * The stock body -- position transform, lighting into the colour output, fog -- is untouched; the
     * texcoord0 output keeps feeding layer A's UV (identical values now sourced from stream 1). "" when
     * the shape does not match (unknown extra inputs, no free output registers, wrong profile).
     */
    std::string BuildLayeredVsText(const std::string& text, VsMapping& map)
    {
        if (text.find("vs_3_0") == std::string::npos) return {};

        bool usedTexUsage[16] = {};
        int maxIn = -1, maxOut = -1;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            DclInfo d;
            if (ParseDcl(text.substr(pos, eol - pos), d))
            {
                if (d.regClass == 'v')
                {
                    // Only the four inputs our vertex declaration feeds may appear; anything else means
                    // this is not the plain single-layer VS and the batch draws stock.
                    const bool ok = (d.usage == "position" && d.index == 0) ||
                                    (d.usage == "normal" && d.index == 0) ||
                                    (d.usage == "color" && d.index == 0) ||
                                    (d.usage == "texcoord" && d.index == 0);
                    if (!ok) return {};
                    if (d.reg > maxIn) maxIn = d.reg;
                }
                else if (d.regClass == 'o')
                {
                    if (d.usage == "texcoord" && d.index < 16) usedTexUsage[d.index] = true;
                    if (d.reg > maxOut) maxOut = d.reg;
                }
            }
            pos = eol + 1;
        }
        if (maxIn < 0 || maxOut < 0 || maxOut + 4 > 11) return {};

        int chosen[4], n = 0;
        for (int i = 1; i < 15 && n < 4; ++i)
            if (!usedTexUsage[i]) chosen[n++] = i;
        if (n < 4) return {};
        map.uv[0] = chosen[0]; map.uv[1] = chosen[1]; map.uv[2] = chosen[2]; map.weights = chosen[3];

        const int vB = maxIn + 1, vC = maxIn + 2, vD = maxIn + 3, vW = maxIn + 4;
        const int oB = maxOut + 1, oC = maxOut + 2, oD = maxOut + 3, oW = maxOut + 4;

        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "dcl_texcoord1 v%d\n"
                      "dcl_texcoord2 v%d\n"
                      "dcl_texcoord3 v%d\n"
                      "dcl_color1 v%d\n"
                      "dcl_texcoord%d o%d.xy\n"
                      "dcl_texcoord%d o%d.xy\n"
                      "dcl_texcoord%d o%d.xy\n"
                      "dcl_texcoord%d o%d\n",
                      vB, vC, vD, vW,
                      map.uv[0], oB, map.uv[1], oC, map.uv[2], oD, map.weights, oW);

        const size_t ins = AfterLastDcl(text);
        if (ins == std::string::npos) return {};
        std::string out = text;
        out.insert(ins, buf);

        std::snprintf(buf, sizeof buf,
                      "\nmov o%d.xy, v%d\n"
                      "mov o%d.xy, v%d\n"
                      "mov o%d.xy, v%d\n"
                      "mov o%d, v%d\n",
                      oB, vB, oC, vC, oD, vD, oW, vW);
        out += buf;
        return out;
    }

    // ------------------------------------------------------------------ the pixel-shader patch
    /**
     * @brief Patches a stock single-layer ps_3_0: the single diffuse fetch becomes the four-layer
     *        height blend + tint, writing the SAME destination register, so the stock combine
     *        (vertex colour, lighting, fog, alpha) runs unchanged on the blended texel.
     *
     * Sampler contract: s0..s3 = diffuse A..D, s4..s7 = height A..D (height in alpha). `tintReg`
     * receives the constant register the bind must fill with {tint.rgb, enable}. "" on shape mismatch.
     */
    std::string BuildLayeredPsText(const std::string& text, const VsMapping& map, int& tintReg)
    {
        if (text.find("ps_3_0") == std::string::npos) return {};
        for (unsigned s = 1; s <= 7; ++s)
        {
            char tok[16];
            std::snprintf(tok, sizeof tok, "dcl_2d s%u", s);
            if (text.find(tok) != std::string::npos) return {};   // not the single-sampler shape
        }

        const TexldLine diffuse = FindTexld(text, 0);
        if (diffuse.at == std::string::npos) return {};
        if (diffuse.dest.find('.') != std::string::npos) return {}; // masked dest: unexpected shape

        const int maxIn = MaxRegister(text, 'v');
        const int maxTmp = MaxRegister(text, 'r');
        const int maxConst = MaxRegister(text, 'c');
        if (maxIn < 0 || maxIn + 4 > 9) return {};
        if (maxTmp + 8 > 31) return {};
        if (maxConst + 2 > 223) return {};

        const int vB = maxIn + 1, vC = maxIn + 2, vD = maxIn + 3, vW = maxIn + 4;
        const int cL = maxConst + 1;   // literals: x = height floor, y = 1
        tintReg      = maxConst + 2;   // uploaded per batch: rgb = tint colour, w = enable
        const int b0 = maxTmp + 1, b1 = maxTmp + 2, b2 = maxTmp + 3, b3 = maxTmp + 4;
        const int bH = maxTmp + 5, bW = maxTmp + 6, bK = maxTmp + 7, bS = maxTmp + 8;

        std::string out = text;

        // The blend block, replacing the one stock diffuse fetch. Ordered exactly as the modern branch
        // computes it: heights -> weights -> peak-relative blend -> renormalise -> diffuse sum -> tint.
        char blk[2048];
        std::snprintf(blk, sizeof blk,
            "texld r%d, %s, s4\n"
            "texld r%d, v%d, s5\n"
            "texld r%d, v%d, s6\n"
            "texld r%d, v%d, s7\n"
            "mov r%d.x, r%d.w\n"
            "mov r%d.y, r%d.w\n"
            "mov r%d.z, r%d.w\n"
            "mov r%d.w, r%d.w\n"
            "max r%d, r%d, c%d.x\n"
            "mov r%d.xyz, v%d\n"
            "dp3_sat r%d.x, v%d, c%d.y\n"
            "add r%d.w, -r%d.x, c%d.y\n"
            "mul r%d, r%d, r%d\n"
            "max r%d.x, r%d.x, r%d.y\n"
            "max r%d.x, r%d.x, r%d.z\n"
            "max r%d.x, r%d.x, r%d.w\n"
            "mad_sat r%d, -r%d, r%d, r%d.x\n"
            "add r%d, -r%d, c%d.y\n"
            "mul r%d, r%d, r%d\n"
            "dp4 r%d.x, r%d, c%d.y\n"
            "rcp r%d.x, r%d.x\n"
            "mul r%d, r%d, r%d.x\n"
            "texld r%d, %s, s0\n"
            "texld r%d, v%d, s1\n"
            "texld r%d, v%d, s2\n"
            "texld r%d, v%d, s3\n"
            "mul %s, r%d, r%d.x\n"
            "mad %s, r%d, r%d.y, %s\n"
            "mad %s, r%d, r%d.z, %s\n"
            "mad %s, r%d, r%d.w, %s\n"
            "mul r%d.x, v%d.w, c%d.w\n"
            "lrp r%d.xyz, r%d.x, c%d, %s\n"
            "mov %s.xyz, r%d",
            b0, diffuse.coord.c_str(),
            b1, vB,
            b2, vC,
            b3, vD,
            bH, b0,
            bH, b1,
            bH, b2,
            bH, b3,
            bH, bH, cL,
            bW, vW,
            bS, vW, cL,
            bW, bS, cL,
            bK, bW, bH,
            bS, bK, bK,
            bS, bS, bK,
            bS, bS, bK,
            bW, bW, bH, bS,
            bW, bW, cL,
            bW, bW, bK,
            bS, bW, cL,
            bS, bS,
            bW, bW, bS,
            b0, diffuse.coord.c_str(),
            b1, vB,
            b2, vC,
            b3, vD,
            diffuse.dest.c_str(), b0, bW,
            diffuse.dest.c_str(), b1, bW, diffuse.dest.c_str(),
            diffuse.dest.c_str(), b2, bW, diffuse.dest.c_str(),
            diffuse.dest.c_str(), b3, bW, diffuse.dest.c_str(),
            bS, vW, tintReg,
            b0, bS, tintReg, diffuse.dest.c_str(),
            diffuse.dest.c_str(), b0);
        out.replace(diffuse.at, diffuse.len, blk);

        // New inputs (the VS pair's added outputs) and the seven extra samplers, after the last dcl;
        // the literal register in canonical position, right after the version line.
        char dcls[512];
        std::snprintf(dcls, sizeof dcls,
                      "dcl_texcoord%d v%d.xy\n"
                      "dcl_texcoord%d v%d.xy\n"
                      "dcl_texcoord%d v%d.xy\n"
                      "dcl_texcoord%d v%d\n"
                      "dcl_2d s1\ndcl_2d s2\ndcl_2d s3\ndcl_2d s4\ndcl_2d s5\ndcl_2d s6\ndcl_2d s7\n",
                      map.uv[0], vB, map.uv[1], vC, map.uv[2], vD, map.weights, vW);
        const size_t ins = AfterLastDcl(out);
        if (ins == std::string::npos) return {};
        out.insert(ins, dcls);

        const size_t ver = out.find("ps_3_0");
        size_t verEol = out.find('\n', ver);
        if (verEol == std::string::npos) return {};
        char defLine[64];
        std::snprintf(defLine, sizeof defLine, "def c%d, 0.004, 1, 0, 0\n", cL);
        out.insert(verEol + 1, defLine);
        return out;
    }

    // ------------------------------------------------------------------ pair build + cache
    const PatchedPair* GetPatchedPair(void* stockVs, void* stockPs)
    {
        const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(stockVs)) << 32) |
                             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stockPs));
        auto it = g_pairs.find(key);
        if (it != g_pairs.end())
            return it->second.vs ? &it->second : nullptr;

        PatchedPair pair;
        auto fail = [&]() -> const PatchedPair* {
            g_pairs.emplace(key, PatchedPair{});   // negative-cache: retry costs nothing
            g_statPatchFail.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        };

        const auto* vsCode = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(stockVs) + shoff::kCgxShaderBytePtr);
        const uint32_t vsLen = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(stockVs) + shoff::kCgxShaderByteLen);
        const auto* psCode = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(stockPs) + shoff::kCgxShaderBytePtr);
        const uint32_t psLen = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(stockPs) + shoff::kCgxShaderByteLen);
        if (!vsCode || !psCode || vsLen < 8 || psLen < 8 || vsLen > 0x20000 || psLen > 0x20000)
            return fail();

        uint32_t vsVer = 0, psVer = 0;
        std::memcpy(&vsVer, vsCode, 4);
        std::memcpy(&psVer, psCode, 4);
        if (vsVer != 0xFFFE0300u || psVer != 0xFFFF0300u)
        {
            // Pre-SM3 profile in use: the patchers only produce the 3_0 forms. Logged once per
            // version pair so the gap is visible instead of silent.
            const uint32_t sig = (vsVer & 0xFFFF) | ((psVer & 0xFFFF) << 16);
            if (g_versionLogged != sig)
            {
                g_versionLogged = sig;
                WLOG_WARN("wmo-layered: unsupported shader profile (vs %04x ps %04x), drawing stock",
                          vsVer & 0xFFFF, psVer & 0xFFFF);
            }
            return fail();
        }

        const std::string vsText = sp::Disassemble(vsCode, vsLen);
        const std::string psText = sp::Disassemble(psCode, psLen);
        if (vsText.empty() || psText.empty()) return fail();

        VsMapping map{};
        const std::string vsPatched = BuildLayeredVsText(vsText, map);
        if (vsPatched.empty()) return fail();
        int tintReg = -1;
        const std::string psPatched = BuildLayeredPsText(psText, map, tintReg);
        if (psPatched.empty()) return fail();

        ID3DBlob* vsBlob = sp::Assemble(vsPatched, "wxlLayeredVs");
        if (!vsBlob) return fail();
        ID3DBlob* psBlob = sp::Assemble(psPatched, "wxlLayeredPs");
        if (!psBlob) { vsBlob->Release(); return fail(); }

        pair.vs = sp::MakeWrapper(vsBlob->GetBufferPointer(), static_cast<uint32_t>(vsBlob->GetBufferSize()), true);
        pair.ps = sp::MakeWrapper(psBlob->GetBufferPointer(), static_cast<uint32_t>(psBlob->GetBufferSize()), false);
        vsBlob->Release();
        psBlob->Release();
        if (!pair.vs || !pair.ps) return fail();
        pair.tintReg = tintReg;

        g_pairs.emplace(key, pair);
        g_statPairs.fetch_add(1, std::memory_order_relaxed);
        WLOG_INFO("wmo-layered: patched four-layer VS/PS pair (uv usages %d/%d/%d, weights %d, tint c%d)",
                  map.uv[0], map.uv[1], map.uv[2], map.weights, tintReg);
        return &g_pairs.find(key)->second;
    }

    // ------------------------------------------------------------------ vertex stream + declaration
    IDirect3DVertexDeclaration9* EnsureDeclaration(IDirect3DDevice9* dev)
    {
        if (g_decl) return g_decl;
        // Stream 0 = the group's own vertex buffer: position/normal/colour sit at the same offsets in
        // both stock WMO vertex formats, so one declaration serves either. Stream 1 = the side stream.
        const D3DVERTEXELEMENT9 elems[] = {
            { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
            { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
            { 1, 0,  D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            { 1, 8,  D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
            { 1, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
            { 1, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
            { 1, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    1 },
            D3DDECL_END()
        };
        dev->CreateVertexDeclaration(elems, &g_decl);
        return g_decl;
    }

    IDirect3DVertexBuffer9* EnsureGroupBuffer(IDirect3DDevice9* dev, void* group, const rt::LayeredGroup& gd)
    {
        GroupBuffer& e = g_groupVb[group];
        if (e.vb && e.srcMotv0 == gd.motv[0] && e.srcWeights == gd.weights && e.verts == gd.vertexCount)
            return e.vb;
        if (e.vb) { e.vb->Release(); e = GroupBuffer{}; }

        const uint32_t bytes = gd.vertexCount * kStream1Stride;
        IDirect3DVertexBuffer9* vb = nullptr;
        if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr)) || !vb)
            return nullptr;
        void* dst = nullptr;
        if (FAILED(vb->Lock(0, bytes, &dst, 0)) || !dst)
        {
            vb->Release();
            return nullptr;
        }
        auto* p = static_cast<uint8_t*>(dst);
        for (uint32_t v = 0; v < gd.vertexCount; ++v)
        {
            for (int setIdx = 0; setIdx < 4; ++setIdx)
                std::memcpy(p + v * kStream1Stride + setIdx * 8,
                            static_cast<const uint8_t*>(gd.motv[setIdx]) + static_cast<size_t>(v) * 8, 8);
            std::memcpy(p + v * kStream1Stride + 32,
                        static_cast<const uint8_t*>(gd.weights) + static_cast<size_t>(v) * 4, 4);
        }
        vb->Unlock();
        e = GroupBuffer{ vb, gd.motv[0], gd.weights, gd.vertexCount };
        g_statGroupVb.fetch_add(1, std::memory_order_relaxed);
        return vb;
    }

    // ------------------------------------------------------------------ the draw wrap
    /**
     * @brief One-shot interceptor around the batch's native draw, published by wxl-modern-m2 as wxl.m2draw
     *        (SetOneShotIntercept) since it owns the DrawIndexedPrimitive vtable slot.
     *
     * The client submits raw group-relative indices and lets BaseVertexIndex carry the vertex pool
     * position, so rebasing stream 0 by that amount and zeroing the base makes index j fetch group
     * vertex j on BOTH streams -- which is exactly how the side stream was built. Every touched state
     * is restored before returning.
     */
    long __cdecl DrawLayered(void* devRaw, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc,
                             WXL_M2Draw_DIPFn orig)
    {
        IDirect3DVertexBuffer9* vb1 = g_pendingVb;
        g_pendingVb = nullptr;
        auto* dev = static_cast<IDirect3DDevice9*>(devRaw);
        if (!vb1 || !g_decl || bv < 0)
            return orig(devRaw, pt, bv, mi, nv, si, pc);

        IDirect3DVertexDeclaration9* prevDecl = nullptr;
        dev->GetVertexDeclaration(&prevDecl);
        DWORD prevFvf = 0;
        dev->GetFVF(&prevFvf);
        IDirect3DVertexBuffer9* vb0 = nullptr;
        UINT off0 = 0, stride0 = 0;
        dev->GetStreamSource(0, &vb0, &off0, &stride0);

        long r;
        // The rebase needs stream-offset support (universal on the D3D9 drivers this client meets);
        // if the runtime refuses it, restore and draw stock rather than draw half-bound.
        if (!vb0 || stride0 == 0 ||
            FAILED(dev->SetStreamSource(0, vb0, off0 + static_cast<UINT>(bv) * stride0, stride0)))
        {
            r = orig(devRaw, pt, bv, mi, nv, si, pc);
        }
        else
        {
            dev->SetStreamSource(1, vb1, 0, kStream1Stride);
            dev->SetVertexDeclaration(g_decl);

            r = orig(devRaw, pt, 0, mi, nv, si, pc);

            dev->SetStreamSource(0, vb0, off0, stride0);
            dev->SetStreamSource(1, nullptr, 0, 0);
            if (prevDecl)     dev->SetVertexDeclaration(prevDecl);
            else if (prevFvf) dev->SetFVF(prevFvf);
            g_statBatches.fetch_add(1, std::memory_order_relaxed);
        }

        if (vb0) vb0->Release();
        if (prevDecl) prevDecl->Release();
        return r;
    }

    // ------------------------------------------------------------------ texture resolution
    /// Resolves a texture id to a loaded client texture handle (the same loader path material creation
    /// uses). Null when the id is unknown to the naming authority.
    void* LoadByFdid(uint32_t fdid)
    {
        if (!fdid) return nullptr;
        const char* path = wxl_modern_wmo::ResolveTexture(fdid);
        if (!path || !path[0]) return nullptr;
        return wxl::game::Native<adt::Map_LoadTextureFn>(adt::kMapLoadTexture)(path);
    }

    /// One-time (per material) resolve of the eight layer textures into the snapshot's handle cache.
    bool EnsureMaterialTextures(rt::LayeredMaterial* mat)
    {
        if (!mat->texturesTried)
        {
            mat->texturesTried = true;
            for (int layer = 0; layer < 4; ++layer)
            {
                mat->diffuseTex[layer] = LoadByFdid(mat->diffuseFdid[layer]);
                mat->heightTex[layer]  = LoadByFdid(mat->heightFdid[layer]);
            }
        }
        for (int layer = 0; layer < 4; ++layer)
            if (!mat->diffuseTex[layer] || !mat->heightTex[layer])
                return false;
        return true;
    }
}

namespace wxl::features::wmolayered
{
    void TryBindLayered(void* root, void* group, uint32_t vtxIdx, uint32_t pixIdx, void* collection,
                        void* gxDevice)
    {
        if constexpr (!wxl_modern_wmo::kEnabled)
            return;

        // Consume the latch immediately, before any other check: hkCullBatch has no way to know which
        // effect-bind call is really "the one that follows", so a stale value must never survive to be
        // read by a LATER, unrelated bind -- see ClearCurrentBatch()'s own doc comment (confirmed off a
        // real crash dump: a stale batch from an unloaded WMO group read as live).
        const void* batch = rt::CurrentBatch();
        rt::ClearCurrentBatch();
        if (!batch || !gxDevice) return;
        rt::LayeredMaterial* mat = rt::FindLayeredMaterial(root, rt::BatchMaterialIndex(batch));
        if (!mat) return;

        rt::LayeredGroup gd{};
        if (!rt::GetLayeredGroup(group, gd))
            return;

        auto* dev = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!dev || !EnsureDeclaration(dev)) return;

        if (!EnsureMaterialTextures(mat))
        {
            g_statTexMissing.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        void* stockVs = *reinterpret_cast<void**>(static_cast<uint8_t*>(collection) +
                                                  vtxIdx * 4u + shoff::kCollectionVtxSlots);
        void* stockPs = *reinterpret_cast<void**>(static_cast<uint8_t*>(collection) +
                                                  pixIdx * 4u + shoff::kCollectionPixSlots);
        if (!stockVs || !stockPs) return;
        const PatchedPair* pair = GetPatchedPair(stockVs, stockPs);
        if (!pair) return;

        IDirect3DVertexBuffer9* vb1 = EnsureGroupBuffer(dev, group, gd);
        if (!vb1) return;

        // Resolve the engine texture objects up front; nothing is bound until all eight are live, so a
        // failed bind leaves the client's own state untouched.
        auto resolveTex = wxl::game::Native<m2off::M2_TexResolveFn>(m2off::kTexResolve);
        void* gxTex[8];
        for (int layer = 0; layer < 4; ++layer)
        {
            gxTex[layer]     = resolveTex(mat->diffuseTex[layer], 0, 0);
            gxTex[4 + layer] = resolveTex(mat->heightTex[layer], 0, 0);
            if (!gxTex[layer] || !gxTex[4 + layer])
            {
                g_statTexMissing.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        // Bind through the engine's own paths: texture states s0..s7 (flushed with the draw exactly
        // like the client's stage-0 bind), the shader slots, and the tint constant.
        auto gxSet = reinterpret_cast<GxSetFn>(shoff::kGxStateSet);
        auto setWrap = wxl::game::Native<gxoff::GxTexSetWrapFn>(gxoff::kGxTexSetWrap);
        const int wrapU = (mat->flags & wmo::kMomtFlagClampS) ? 0 : 1;
        const int wrapV = (mat->flags & wmo::kMomtFlagClampT) ? 0 : 1;
        for (int s = 0; s < 8; ++s)
        {
            setWrap(gxTex[s], wrapU, wrapV);
            gxSet(gxDevice, nullptr, shoff::kStateTexture0 + s, gxTex[s]);
        }
        g_stagesBound = true;

        gxSet(gxDevice, nullptr, shoff::kStateVertexShader, pair->vs);
        gxSet(gxDevice, nullptr, shoff::kStatePixelShader, pair->ps);

        const uint32_t tint = mat->tintBgra;
        const float tintVec[4] = {
            static_cast<float>((tint >> 16) & 0xFF) / 255.0f,
            static_cast<float>((tint >> 8) & 0xFF) / 255.0f,
            static_cast<float>(tint & 0xFF) / 255.0f,
            (tint & 0x00FFFFFF) != 0 ? 1.0f : 0.0f,   // black tint reads as "no tint recorded"
        };
        wxl::game::Native<shoff::ShaderConstantsSetHelperFn>(shoff::kShaderConstantsSet)(
            4, pair->tintReg, tintVec, 1);

        g_pendingVb = vb1;
        if (const WXL_M2DrawApi* m2draw = wxl_modern_wmo::M2Draw())
            m2draw->SetOneShotIntercept(&DrawLayered);
    }

    void OnLeafEnd(void* gxDevice)
    {
        // Disarm a bind whose draw never issued, and clear the extra texture stages so no layer
        // texture outlives the leaf in the engine's state cache. The disarm is unconditional -- the
        // m2draw slot cannot be read back across the ABI, so this is a harmless no-op when the draw
        // already consumed the one-shot (or wxl-modern-m2 hasn't published yet).
        if (const WXL_M2DrawApi* m2draw = wxl_modern_wmo::M2Draw())
            m2draw->SetOneShotIntercept(nullptr);
        g_pendingVb = nullptr;
        if (!g_stagesBound || !gxDevice) return;
        g_stagesBound = false;
        auto gxSet = reinterpret_cast<GxSetFn>(shoff::kGxStateSet);
        for (int s = 1; s < 8; ++s)
            gxSet(gxDevice, nullptr, shoff::kStateTexture0 + s, nullptr);
    }
}

namespace wxl::runtime::wmolayered
{
    Stats GetStats()
    {
        Stats s{};
        s.pairsPatched    = g_statPairs.load(std::memory_order_relaxed);
        s.patchFailures   = g_statPatchFail.load(std::memory_order_relaxed);
        s.groupBuffers    = g_statGroupVb.load(std::memory_order_relaxed);
        s.batchesDrawn    = g_statBatches.load(std::memory_order_relaxed);
        s.texturesMissing = g_statTexMissing.load(std::memory_order_relaxed);
        return s;
    }
}
