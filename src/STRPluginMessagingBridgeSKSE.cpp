#include "SKSEPluginVersionCompat.h"

#include <cstdint>
#include <cstdio>

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
    FILE* file = nullptr;
    fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "a");
    if (file != nullptr) {
        std::fprintf(file, "STRPluginMessagingBridge v0.4.1: SKSEPlugin_Load entered\n");
        std::fclose(file);
    }

    // The transport hooks are initialized later by STRPluginMessagingAPI via
    // STRPM_QueryTransportInterface.
    return true;
}
