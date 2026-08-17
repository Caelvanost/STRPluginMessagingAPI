#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <ShlObj.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

// The STR runtime is mapped/unmapped while the game is starting. The bridge
// resolver scans process memory during that window, so a region that was
// readable when VirtualQuery enumerated it can disappear before memcmp reaches
// it. Keep comparisons fail-closed instead of letting an AV escape into Skyrim.
inline int STRPMSafeMemcmp(
    const void* left,
    const void* right,
    std::size_t count) noexcept
{
    if (count == 0)
        return 0;
    if (left == nullptr || right == nullptr)
        return left == right ? 0 : (left != nullptr ? 1 : -1);

    // If a page/region faulted moments ago, skip it for the rest of the current
    // resolver pass. The short expiry lets a later 750 ms bootstrap retry scan a
    // newly remapped STR region normally.
    thread_local std::uintptr_t badBegin = 0;
    thread_local std::uintptr_t badEnd = 0;
    thread_local ULONGLONG badAt = 0;

    const auto leftAddress = reinterpret_cast<std::uintptr_t>(left);
    const auto rightAddress = reinterpret_cast<std::uintptr_t>(right);
    if (badBegin != 0) {
        const auto now = GetTickCount64();
        if (now - badAt <= 250 &&
            ((leftAddress >= badBegin && leftAddress < badEnd) ||
             (rightAddress >= badBegin && rightAddress < badEnd))) {
            return 1;
        }
        if (now - badAt > 250) {
            badBegin = 0;
            badEnd = 0;
            badAt = 0;
        }
    }

#if defined(_MSC_VER)
    __try {
#endif
        const auto* lhs = static_cast<const unsigned char*>(left);
        const auto* rhs = static_cast<const unsigned char*>(right);
        for (std::size_t index = 0; index < count; ++index) {
            if (lhs[index] != rhs[index])
                return lhs[index] < rhs[index] ? -1 : 1;
        }
        return 0;
#if defined(_MSC_VER)
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(left, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.RegionSize != 0) {
            badBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            badEnd = badBegin + static_cast<std::size_t>(mbi.RegionSize);
        } else {
            constexpr std::uintptr_t kPageMask = ~std::uintptr_t{ 0xFFF };
            badBegin = leftAddress & kPageMask;
            badEnd = badBegin + 0x1000;
        }
        badAt = GetTickCount64();
        return 1;
    }
#endif
}

// STRPluginMessagingBridge.cpp uses std::memcmp. The force-included header is
// parsed before <cstring> is re-included by the source, so expose the guarded
// implementation in std and redirect only this bridge target's memcmp tokens.
namespace std
{
    using ::STRPMSafeMemcmp;
}

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

                wchar_t wideMode[8]{};
                if (mode != nullptr) {
                    std::size_t index = 0;
                    for (; mode[index] != '\0' && index + 1 < std::size(wideMode); ++index)
                        wideMode[index] = static_cast<wchar_t>(static_cast<unsigned char>(mode[index]));
                    wideMode[index] = L'\0';
                }
                if (wideMode[0] == L'\0') {
                    wideMode[0] = L'a';
                    wideMode[1] = L'\0';
                }

                const auto result = _wfopen_s(file, logPath.c_str(), wideMode);
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
#define memcmp STRPMSafeMemcmp
