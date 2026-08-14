#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr char kTargetBuild[] = "Skyrim Together Reborn 1.8.0";
    constexpr char kSendChatAnchor[] = "Send chat message of type {}: '{}' ";

    std::mutex g_lock;
    STRPM::ReceiveCallback g_callback = nullptr;
    void* g_userData = nullptr;
    std::vector<std::uintptr_t> g_anchorAddresses;
    std::vector<std::uintptr_t> g_xrefs;

    void Log(const char* fmt, ...)
    {
        FILE* file = nullptr;
        fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "a");
        if (!file)
            return;

        va_list args;
        va_start(args, fmt);
        vfprintf(file, fmt, args);
        va_end(args);
        fputc('\n', file);
        fclose(file);
    }

    bool IsReadable(DWORD protect)
    {
        if ((protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS)
            return false;
        const DWORD p = protect & 0xFF;
        return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
               p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    }

    bool IsExecutable(DWORD protect)
    {
        const DWORD p = protect & 0xFF;
        return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
               p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    }

    void FindAnchorCopies()
    {
        g_anchorAddresses.clear();
        const auto* needle = reinterpret_cast<const std::uint8_t*>(kSendChatAnchor);
        constexpr std::size_t needleSize = sizeof(kSendChatAnchor) - 1;

        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        auto current = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        const auto end = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

        while (current < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi)) != sizeof(mbi))
                break;

            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto size = static_cast<std::size_t>(mbi.RegionSize);
            if (mbi.State == MEM_COMMIT && IsReadable(mbi.Protect) && size >= needleSize)
            {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(base);
                for (std::size_t i = 0; i + needleSize <= size; ++i)
                {
                    if (std::memcmp(bytes + i, needle, needleSize) == 0)
                        g_anchorAddresses.push_back(base + i);
                }
            }
            current = base + size;
        }
    }

    void FindRipRelativeXrefs()
    {
        g_xrefs.clear();
        if (g_anchorAddresses.empty())
            return;

        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        auto current = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        const auto end = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

        while (current < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi)) != sizeof(mbi))
                break;

            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto size = static_cast<std::size_t>(mbi.RegionSize);
            if (mbi.State == MEM_COMMIT && IsExecutable(mbi.Protect) && size >= 7)
            {
                const auto* code = reinterpret_cast<const std::uint8_t*>(base);
                for (std::size_t i = 0; i + 7 <= size; ++i)
                {
                    // x64 LEA reg,[RIP+disp32]. The ModRM values below cover RCX/RDX/R8/R9-style
                    // compiler temporaries commonly used for string arguments.
                    if (code[i] != 0x48 || code[i + 1] != 0x8D)
                        continue;
                    const std::uint8_t modrm = code[i + 2];
                    if ((modrm & 0xC7) != 0x05)
                        continue;

                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, code + i + 3, sizeof(displacement));
                    const auto target = base + i + 7 + displacement;
                    for (const auto anchor : g_anchorAddresses)
                    {
                        if (target == anchor)
                        {
                            g_xrefs.push_back(base + i);
                            break;
                        }
                    }
                }
            }
            current = base + size;
        }
    }

    void ProbeSTR180()
    {
        FindAnchorCopies();
        FindRipRelativeXrefs();
        Log("STRPM bridge probe target: %s", kTargetBuild);
        Log("send-chat anchor copies: %zu", g_anchorAddresses.size());
        for (const auto address : g_anchorAddresses)
            Log("  anchor = 0x%llX", static_cast<unsigned long long>(address));
        Log("send-chat RIP xrefs: %zu", g_xrefs.size());
        for (const auto address : g_xrefs)
            Log("  xref = 0x%llX", static_cast<unsigned long long>(address));
    }

    STRPM::Result STRPM_CALL Start(STRPM::ReceiveCallback callback, void* userData)
    {
        if (!callback)
            return STRPM::Result::kInvalidArgument;
        {
            std::scoped_lock lock(g_lock);
            g_callback = callback;
            g_userData = userData;
        }
        ProbeSTR180();
        // The probe deliberately does not claim connectivity until the send and receive
        // entry points have both been resolved and validated for the public 1.8.0 binary.
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL Stop()
    {
        std::scoped_lock lock(g_lock);
        g_callback = nullptr;
        g_userData = nullptr;
        return STRPM::Result::kOk;
    }

    STRPM::Result STRPM_CALL Send(const char*, STRPM::Target, const void*, std::size_t, std::uint32_t)
    {
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL GetLocalConnectionID(STRPM::ConnectionID*)
    {
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL SetLocalDisplayName(const char*)
    {
        return STRPM::Result::kOk;
    }

    const STRPM::TransportInterface g_interface{
        STRPM::kTransportInterfaceVersion,
        &Start,
        &Stop,
        &Send,
        &GetLocalConnectionID,
        &SetLocalDisplayName
    };
}

STRPM_EXPORT STRPM::Result STRPM_CALL STRPM_QueryTransportInterface(
    std::uint32_t requestedVersion,
    const STRPM::TransportInterface** outInterface)
{
    if (!outInterface)
        return STRPM::Result::kInvalidArgument;
    *outInterface = nullptr;
    if (requestedVersion != STRPM::kTransportInterfaceVersion)
        return STRPM::Result::kUnsupportedVersion;
    *outInterface = &g_interface;
    return STRPM::Result::kOk;
}
