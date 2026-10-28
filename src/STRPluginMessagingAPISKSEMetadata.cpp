#include "SKSEPluginVersionCompat.h"
#include "STRPMProxyResolverRuntime.h"

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_10_1,
    "STRPluginMessagingAPI",
    "Caelvanost",
    "",
    0,
    0,
    { STRPMSKSE::kRuntime_1_7_104, 0 },
    0
};
