#pragma once

#include <cstdint>

namespace STRPMSKSE
{
    // Mirrors SKSEPluginVersionData from SKSE64 PluginAPI.h.
    // Keep this private to avoid taking a compile-time dependency on SKSE/CommonLib.
    struct PluginVersionData
    {
        static constexpr std::uint32_t kVersion = 1;

        std::uint32_t dataVersion;
        std::uint32_t pluginVersion;
        char name[256];
        char author[256];
        char supportEmail[252];
        std::uint32_t versionIndependenceEx;
        std::uint32_t versionIndependence;
        std::uint32_t compatibleVersions[16];
        std::uint32_t seVersionRequired;
    };

    // Runtime reported by the user's SKSE 2.2.6 log:
    // 0x01064920 == SkyrimSE.exe 1.6.1170.
    inline constexpr std::uint32_t kRuntime_1_6_1170 = 0x01064920;

    // Keep the v0.5.1 constant because the non-packaged diagnostic regression
    // target still carries its original metadata while v0.6.0 ships API+bridge.
    inline constexpr std::uint32_t kPluginVersion_0_5_1 = 0x00050001;
    inline constexpr std::uint32_t kPluginVersion_0_6_0 = 0x00060000;
}
