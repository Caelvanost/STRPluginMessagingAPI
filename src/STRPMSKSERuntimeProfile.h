#pragma once

// Compile-time SKSE runtime profile for the packaged DLL variant.
// The public STRPM API and wire protocol remain identical across profiles.

#if defined(STRPM_RUNTIME_1_6_1170)
#define STRPM_SKSE_COMPATIBLE_VERSION_LIST \
    { STRPMSKSE::kRuntime_1_6_1170, 0 }
#define STRPM_SKSE_RUNTIME_LABEL "Skyrim 1.6.1170"
#elif defined(STRPM_RUNTIME_1_7_104)
#define STRPM_SKSE_COMPATIBLE_VERSION_LIST \
    { STRPMSKSE::kRuntime_1_7_104, 0 }
#define STRPM_SKSE_RUNTIME_LABEL "Skyrim 1.7.104"
#else
#error "STRPM runtime profile is not defined"
#endif
