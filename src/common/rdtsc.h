// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/types.h"
#include "common/logging/log.h"

namespace Common {

#ifdef __x86_64__

#ifdef _MSC_VER
// MSVC
__forceinline static u64 FencedRDTSC() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
}
#else
// Linux/Mac
static inline u64 FencedRDTSC() {
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
}
#endif

#else
// ARM
static inline u64 FencedRDTSC() 
{
    LOG_INFO(Core_Linker, "TODO: ARM FencedRDTSC");
    return 0;
}
#endif

u64 EstimateRDTSCFrequency();

} // namespace Common