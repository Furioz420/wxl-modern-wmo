// wxl-modern-wmo: native modern-WMO reader (tag-driven chunk walkers, material/shader remap, outdoor gate,
// composite/layered shader fixes), as an out-of-core extension. Entry point.
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

#include "ExtensionApi.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-modern-wmo",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    wxl_modern_wmo::g_api = api;

    // wmo-spawn and wmo-group-parse are unconditional (publish OnWmoRootLoad/OnWmoGroupLoad regardless
    // of kEnabled -- always were, even back when these were core WXL_REGISTER_FEATURE("...", true, ...)).
    wxl_modern_wmo::InstallWmoSpawn();
    wxl_modern_wmo::InstallWmoGroupParse();

    if constexpr (wxl_modern_wmo::kEnabled)
    {
        wxl_modern_wmo::InstallCollisionRay();
        wxl_modern_wmo::InstallWmoNative();
        if constexpr (wxl_modern_wmo::kAsyncEnabled)
            wxl_modern_wmo::InstallWmoAsync();
        wxl_modern_wmo::InstallOutdoorGate();
        wxl_modern_wmo::InstallCompositeShader();
    }

    api->Log(WXL_LOG_INFO, "wxl-modern-wmo", "native modern-WMO reader active");
    return 1;
}
