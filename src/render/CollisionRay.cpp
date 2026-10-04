// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include "../ExtensionApi.hpp"
#include "CollisionRay.hpp"
#include "offsets/game/Movement.hpp"
#include <atomic>
namespace {
using TraceFn=wxl::offsets::game::movement::TraceLineFn;
TraceFn original=nullptr;
std::atomic<unsigned> rejectedCount{0};
char __cdecl CheckedTrace(float* end,float* start,float* impact,float* fraction,
                          uint32_t flags,uint32_t extra) {
    bool rejected=false;
    const char result=wxl_modern_wmo::collision::Trace(original,end,start,impact,fraction,flags,extra,rejected);
    if(rejected) {
        const auto count=rejectedCount.fetch_add(1,std::memory_order_relaxed)+1;
        if(count<=4 || (count & (count-1))==0)
            WLOG_WARN("world-ray: rejected nonfinite/null endpoint before native collision (count=%u flags=%08X)",count,flags);
    }
    return result;
}
}
namespace wxl_modern_wmo {
bool InstallCollisionRay() {
    const bool ok=HookAttachByName("World.MapRayIntersect",&CheckedTrace,&original)!=0;
    if(ok) WLOG_INFO("world-ray: finite endpoint guard installed; valid native queries unchanged");
    else WLOG_ERROR("world-ray: finite endpoint guard installation failed");
    return ok;
}
}
