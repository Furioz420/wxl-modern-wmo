// wxl-modern-wmo: definition of the extension-wide service table pointer.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

namespace wxl_modern_wmo
{
    const WXL_Api* g_api = nullptr;
    const WXL_FdidApi* g_fdid = nullptr;
    const WXL_M2DrawApi* g_m2draw = nullptr;
    const WXL_LoadPoolApi* g_loadPool = nullptr;
}
