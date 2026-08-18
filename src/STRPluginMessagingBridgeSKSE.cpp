#include "SKSEPluginVersionCompat.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "STRPMProxyResolverBridge.h"
#include "STRPMProxyResolverTrace.h"

struct SKSEInterface;

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_8_0,
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
    FILE* file = nullptr;
    fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "a");
    if (file != nullptr)
    {
        std::fprintf(file, "STRPluginMessagingBridge v0.8.0: SKSEPlugin_Load entered\n");
        std::fclose(file);
    }

    // Keep the validated v0.7.0 transport/suppression path unchanged. The
    // v0.8.0 ProxyResolver is isolated here and observes canonical STR player
    // lifecycle data without scanning Skyrim ProcessLists or guessing actors.
    STRPMProxyResolverBridge::Start();
    STRPMProxyResolverTrace::Start();

    // Transport hooks are initialized later by STRPluginMessagingAPI through
    // STRPM_QueryTransportInterface.
    return true;
}
