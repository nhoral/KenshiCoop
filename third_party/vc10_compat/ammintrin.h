/*
 * Compatibility shim for the VC++ 2010 SP1 *compiler-only* update (KB2519277).
 *
 * That update ships a <intrin.h> which unconditionally does `#include <ammintrin.h>`,
 * but ammintrin.h itself only ships with the full Visual Studio 2010 SP1 IDE - so it
 * is absent on a "Windows SDK 7.1 + compiler update" toolchain (no full VS2010).
 *
 * intrin.h already declares the AMD SSE4a intrinsics it references (_mm_stream_sd,
 * _mm_stream_ss, _mm_extract_si64, _mm_extracti_si64, _mm_insert_si64,
 * _mm_inserti_si64) via its own __MACHINEI declarations, so this header only needs
 * to exist to satisfy the include. This project uses no SSE4a intrinsics.
 */
#pragma once
