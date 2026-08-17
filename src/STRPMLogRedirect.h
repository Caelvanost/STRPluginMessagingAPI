#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

inline errno_t STRPMRedirectFopen(
    FILE** file,
    const char* filename,
    const char* mode) noexcept
{
    constexpr char kLegacyBridgeLogPath[] =
        "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log";

    if (file != nullptr &&
        filename != nullptr &&
        std::strcmp(filename, kLegacyBridgeLogPath) == 0)
    {
        PWSTR documents = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(
                FOLDERID_Documents,
                KF_FLAG_DEFAULT,
                nullptr,
                &documents)) &&
            documents != nullptr)
        {
            std::filesystem::path logDirectory(documents);
            CoTaskMemFree(documents);

            logDirectory /= L"My Games";
            logDirectory /= L"Skyrim Special Edition";
            logDirectory /= L"SKSE";

            std::error_code error;
            std::filesystem::create_directories(logDirectory, error);
            if (!error)
            {
                const auto logPath = logDirectory / L"STRPluginMessagingBridge.log";
                const auto result = _wfopen_s(file, logPath.c_str(), L"a");
                if (result == 0)
                    return 0;
            }
        }
    }

    return ::fopen_s(file, filename, mode);
}

// Force-included only for STRPluginMessagingBridge. This redirects the existing
// bridge/receiver/SKSE log writes without modifying their runtime hook logic.
#define fopen_s STRPMRedirectFopen
