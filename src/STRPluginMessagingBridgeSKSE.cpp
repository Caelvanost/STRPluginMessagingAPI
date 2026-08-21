#include "SKSEPluginVersionCompat.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "STRPMProxyResolverBridge.h"
#include "STRPMProxyResolverBootstrapV2.h"
#include "STRPMProxyResolverTrace.h"

struct SKSEInterface;

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_9_0,
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
        std::fprintf(file, "STRPluginMessagingBridge v0.9.0: SKSEPlugin_Load entered\n");
        std::fclose(file);
    }

    // Keep the validated transport/proxy resolver path isolated here. v0.9.0
    // changes the receive execution model so STRPM envelope parsing and consumer
    // dispatch no longer run from the TransportService::OnConsume VEH.
    STRPMProxyResolverBootstrapV2::Start();
    STRPMProxyResolverTrace::Start();

    // Transport hooks are initialized later by STRPluginMessagingAPI through
    // STRPM_QueryTransportInterface.
    return true;
}
