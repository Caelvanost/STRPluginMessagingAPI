#include "SKSEPluginVersionCompat.h"
#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#include <Windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "STRPMProxyResolverBridge.h"
#include "STRPMProxyResolverBootstrapV2.h"
#include "STRPMProxyResolverTrace.h"

struct SKSEInterface;

namespace
{
    constexpr char kIdentityChannel[] = "strpm.identity.v1";
    constexpr auto kIdentityRetryDelay = std::chrono::milliseconds(250);
    constexpr auto kIdentityHeartbeatInterval = std::chrono::seconds(2);

    std::jthread g_identityWorker;

    void LogIdentity(const char* format, ...) noexcept
    {
        FILE* file = nullptr;
        fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "a");
        if (!file)
            return;

        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);
        std::fputc('\n', file);
        std::fclose(file);
    }

    const STRPM::TransportInterface* QueryTransport() noexcept
    {
        const auto module = GetModuleHandleW(L"STRPluginMessagingBridge.dll");
        if (!module)
            return nullptr;

        const auto raw = GetProcAddress(module, STRPM::kQueryTransportExportName);
        if (!raw)
            return nullptr;

        const auto query = reinterpret_cast<STRPM::QueryTransportInterfaceFn>(raw);
        const STRPM::TransportInterface* transport = nullptr;
        if (query(STRPM::kTransportInterfaceVersion, &transport) != STRPM::Result::kOk ||
            !transport || transport->version != STRPM::kTransportInterfaceVersion)
        {
            return nullptr;
        }
        return transport;
    }

    STRPM::Result SendIdentityAnnouncement() noexcept
    {
        const auto* transport = QueryTransport();
        if (!transport || !transport->send)
            return STRPM::Result::kNotAvailable;

        STRPM::Target target{};
        target.kind = STRPM::TargetKind::kAllPlayers;
        constexpr std::uint32_t flags =
            STRPM::kMessageReliable |
            STRPM::kMessageOrdered;

        return transport->send(
            kIdentityChannel,
            target,
            nullptr,
            0,
            flags);
    }

    bool SleepInterruptible(std::stop_token token, std::chrono::milliseconds duration)
    {
        constexpr auto slice = std::chrono::milliseconds(100);
        auto slept = std::chrono::milliseconds(0);
        while (slept < duration && !token.stop_requested())
        {
            const auto remaining = duration - slept;
            const auto current = remaining < slice ? remaining : slice;
            std::this_thread::sleep_for(current);
            slept += current;
        }
        return !token.stop_requested();
    }

    void IdentityWorker(std::stop_token token)
    {
        bool announcedThisSession = false;
        bool loggedWaiting = false;
        auto nextHeartbeat = std::chrono::steady_clock::time_point{};

        while (!token.stop_requested())
        {
            if (!STRPMProxyResolverBridge::IsSTRSessionConnected())
            {
                announcedThisSession = false;
                loggedWaiting = false;
                nextHeartbeat = {};
                if (!SleepInterruptible(token, kIdentityRetryDelay))
                    return;
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!announcedThisSession || nextHeartbeat.time_since_epoch().count() == 0 || now >= nextHeartbeat)
            {
                const auto result = SendIdentityAnnouncement();
                if (result == STRPM::Result::kOk)
                {
                    if (!announcedThisSession)
                        LogIdentity("ProxyResolver identity bootstrap announcement sent");
                    announcedThisSession = true;
                    loggedWaiting = false;
                    nextHeartbeat = now + kIdentityHeartbeatInterval;
                }
                else
                {
                    if (!loggedWaiting &&
                        result != STRPM::Result::kNotConnected &&
                        result != STRPM::Result::kNotAvailable)
                    {
                        LogIdentity(
                            "ProxyResolver identity bootstrap waiting: result=%u",
                            static_cast<unsigned>(result));
                        loggedWaiting = true;
                    }
                    nextHeartbeat = now + kIdentityRetryDelay;
                }
            }

            if (!SleepInterruptible(token, kIdentityRetryDelay))
                return;
        }
    }
}

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_9_1,
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
        std::fprintf(file, "STRPluginMessagingBridge v0.9.1: SKSEPlugin_Load entered\n");
        std::fclose(file);
    }

    // Keep the validated transport/proxy resolver path isolated here. v0.9.0
    // changes the receive execution model so STRPM envelope parsing and consumer
    // dispatch no longer run from the TransportService::OnConsume VEH.
    STRPMProxyResolverBootstrapV2::Start();
    STRPMProxyResolverTrace::Start();

    // v0.9.1 fixes ProxyResolver's cold-start dependency on consumer traffic.
    // The server relay authenticates every STRPM envelope with sender ConnectionID
    // and PlayerId metadata. Emit a tiny reserved heartbeat after connection so
    // peers can join ConnectionID -> PlayerId to the already observed
    // PlayerId -> local proxy FormID before any consumer sends its first message.
    g_identityWorker = std::jthread(&IdentityWorker);

    // Transport hooks are initialized later by STRPluginMessagingAPI through
    // STRPM_QueryTransportInterface.
    return true;
}
