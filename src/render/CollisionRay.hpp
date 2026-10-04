// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
namespace wxl_modern_wmo::collision {
// Bit classification avoids depending on the caller's floating-point state.
inline bool FinitePoint(const float* point) {
    if(!point) return false;
    for(unsigned i=0;i<3;++i) {
        uint32_t bits; std::memcpy(&bits,point+i,sizeof(bits));
        if((bits & 0x7f800000u)==0x7f800000u) return false;
    }
    return true;
}
template<class Fn>
char Trace(Fn original,float* end,float* start,float* impact,float* fraction,
           uint32_t flags,uint32_t extra,bool& rejected) {
    rejected=!FinitePoint(end)||!FinitePoint(start);
    // No hit; preserve caller-owned output storage, as a failed query does.
    if(rejected) return 0;
    return original(end,start,impact,fraction,flags,extra);
}
}
