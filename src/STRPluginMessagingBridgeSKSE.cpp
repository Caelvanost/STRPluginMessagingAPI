#include "SKSEPluginVersionCompat.h"

#include <cstdint>

struct SKSEInterface;

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_4_1,
    "STRPluginMessagingBridge",
    "Caelvanost",
    "",
    0,
    0,
    { STRPMSKSE::kRuntime_1_6_1170, 0 },
    0
};

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface*)
{
    // The bridge is initialized by STRPluginMessagingAPI through
    // STRPM_QueryTransportInterface. Loading it through SKSE only guarantees a
    // stable module location independent of the process working directory.
    return true;
}
