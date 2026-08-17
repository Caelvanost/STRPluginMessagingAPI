#include "SKSEPluginVersionCompat.h"

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_4_6,
    "STRPluginMessagingAPI",
    "Caelvanost",
    "",
    0,
    0,
    { STRPMSKSE::kRuntime_1_6_1170, 0 },
    0
};
