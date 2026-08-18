#include "SKSEPluginVersionCompat.h"

#include <cstdint>
#include <cstdio>

struct SKSEInterface;

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_7_0,
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
        std::fprintf(file, "STRPluginMessagingBridge v0.7.0: SKSEPlugin_Load entered\n");
        std::fclose(file);
    }

    // v0.7.0 suppresses reserved STRPM chat packets in the validated receive
    // path at TransportService::OnConsume, before STR creates and dispatches a
    // NotifyChatMessageBroadcast. No separate overlay/UI hook is required.
    // Transport hooks are initialized later by STRPluginMessagingAPI through
    // STRPM_QueryTransportInterface.
    return true;
}
