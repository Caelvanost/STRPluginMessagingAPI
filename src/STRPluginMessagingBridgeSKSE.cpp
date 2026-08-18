#include "SKSEPluginVersionCompat.h"
#include "STRPMChatUiSuppress.h"

#include <cstdint>
#include <cstdio>

struct SKSEInterface;

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_6_0,
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
    if (file != nullptr) {
        std::fprintf(file, "STRPluginMessagingBridge v0.6.0: SKSEPlugin_Load entered\n");
        std::fclose(file);
    }

    // UI suppression is independent from the transport hooks. It waits for the
    // mapped STR 1.8.0 runtime and dynamically identifies OverlayService's chat
    // callback before filtering only STRPM|v2| envelopes.
    STRPMChatUiSuppress::Start();

    // The transport hooks are initialized later by STRPluginMessagingAPI via
    // STRPM_QueryTransportInterface.
    return true;
}
