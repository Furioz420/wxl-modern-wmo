// In-memory shader surgery shared by the modern-WMO material fixes: disassemble, reassemble, mint.
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

#include "../ExtensionApi.hpp"

#include "game/Gx.hpp"
#include "offsets/engine/Shader.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <string>

/// Feature-private toolbox for the disassemble -> edit -> reassemble -> wrap pipeline the WMO material
/// fixes share. A patched shader never touches a .bls file: the stock wrapper's bytecode is read out,
/// rewritten as text, reassembled on the live device, and the new bytecode is minted into a minimal
/// GPU-shader-layout wrapper the GxState flush can bind (live handle + created flag + bytecode fields).
namespace wxl::features::wmoshader
{
    // D3DAssemble has no SDK header declaration; both are resolved by name from the delay-loadable
    // compiler module, so an absent d3dcompiler_47 degrades to "draw stock" instead of a load fault.
    typedef HRESULT(WINAPI* PFN_D3DAssemble)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                             UINT, ID3DBlob**, ID3DBlob**);
    typedef HRESULT(WINAPI* PFN_D3DDisassemble)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

    inline HMODULE Compiler()
    {
        HMODULE d = GetModuleHandleA("d3dcompiler_47.dll");
        return d ? d : LoadLibraryA("d3dcompiler_47.dll");
    }

    /** @brief Disassembles device bytecode to text; empty on failure. */
    inline std::string Disassemble(const void* code, uint32_t len)
    {
        HMODULE comp = Compiler();
        auto disasm = reinterpret_cast<PFN_D3DDisassemble>(comp ? GetProcAddress(comp, "D3DDisassemble") : nullptr);
        if (!disasm || !code || !len) return {};
        ID3DBlob* blob = nullptr;
        if (FAILED(disasm(code, len, 0, nullptr, &blob)) || !blob) return {};
        std::string text(static_cast<const char*>(blob->GetBufferPointer()));
        blob->Release();
        return text;
    }

    /** @brief Reassembles shader text; null on failure (the error is logged under `label`). */
    inline ID3DBlob* Assemble(const std::string& text, const char* label)
    {
        HMODULE comp = Compiler();
        auto assemble = reinterpret_cast<PFN_D3DAssemble>(comp ? GetProcAddress(comp, "D3DAssemble") : nullptr);
        if (!assemble) return nullptr;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err  = nullptr;
        const HRESULT hr = assemble(text.c_str(), text.size(), label, nullptr, nullptr, 0, &blob, &err);
        if (FAILED(hr) || !blob)
        {
            WLOG_WARN("%s: reassemble failed: %s", label,
                      err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (blob) blob->Release();
            blob = nullptr;
        }
        if (err) err->Release();
        return blob;
    }

    /**
     * @brief Wraps freshly created device bytecode in the minimal GPU-shader layout the GxState shader
     *        flush reads: live handle +0x20, created flag +0x30, bytecode ptr/len +0x50/+0x4C.
     * @param bytecode  assembled shader bytecode (copied; the caller may release its blob).
     * @param len       bytecode byte length.
     * @param vertex    true mints a vertex shader, false a pixel shader.
     * @return the wrapper, or null when the device is down or creation failed.
     */
    inline void* MakeWrapper(const void* bytecode, uint32_t len, bool vertex)
    {
        namespace shoff = wxl::offsets::engine::shader;
        auto* dev = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!dev) return nullptr;
        void* handle = nullptr;
        if (vertex)
        {
            IDirect3DVertexShader9* vs = nullptr;
            if (FAILED(dev->CreateVertexShader(static_cast<const DWORD*>(bytecode), &vs)) || !vs)
                return nullptr;
            handle = vs;
        }
        else
        {
            IDirect3DPixelShader9* ps = nullptr;
            if (FAILED(dev->CreatePixelShader(static_cast<const DWORD*>(bytecode), &ps)) || !ps)
                return nullptr;
            handle = ps;
        }
        auto* copy = new uint8_t[len];
        std::memcpy(copy, bytecode, len);
        auto* w = new uint8_t[shoff::kCgxShaderWrapBytes]();
        *reinterpret_cast<void**>(w + shoff::kCgxShaderHandle)          = handle;
        *reinterpret_cast<uint32_t*>(w + shoff::kCgxShaderCreated)      = 1;
        *reinterpret_cast<uint32_t*>(w + shoff::kCgxShaderByteLen)      = len;
        *reinterpret_cast<const void**>(w + shoff::kCgxShaderBytePtr)   = copy;
        return w;
    }
}
