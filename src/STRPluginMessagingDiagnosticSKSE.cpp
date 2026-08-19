#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"
#include "SKSEPluginVersionCompat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

struct SKSEInterface;

namespace
{
    constexpr char kDiagnosticChannel[] = "strpm.test";
    constexpr auto kApiRetryDelay = std::chrono::milliseconds(500);
    constexpr auto kConnectionPollDelay = std::chrono::milliseconds(250);
    constexpr auto kConnectionSettleDelay = std::chrono::seconds(1);
    constexpr auto kSendRetryDelay = std::chrono::seconds(2);
    constexpr auto kSendRetryLogInterval = std::chrono::seconds(10);
    constexpr auto kProbeSpacing = std::chrono::seconds(5);
    constexpr auto kHandshakePollDelay = std::chrono::milliseconds(250);
    constexpr auto kProxyPollDelay = std::chrono::milliseconds(250);
    constexpr auto kProxyResolveTimeout = std::chrono::seconds(15);
    constexpr std::uint32_t kProbeFlags =
        STRPM::kMessageReliable |
        STRPM::kMessageOrdered |
        STRPM::kMessageAllowLoopback;

    using IsSTRSessionConnectedFn = bool(STRPM_CALL*)();

    std::mutex g_logMutex;
    std::jthread g_worker;
    STRPM::ListenerHandle g_listener{};
    std::string g_computerName;
    std::atomic_bool g_peerObserved{ false };
    std::atomic_bool g_peerAckObserved{ false };
    std::atomic<STRPM::ConnectionID> g_peerConnectionID{ 0 };

    void Log(const char* fmt, ...)
    {
        std::scoped_lock lock(g_logMutex);

        FILE* file = nullptr;
        fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingDiagnostic.log", "a");
        if (!file)
            return;

        va_list args;
        va_start(args, fmt);
        std::vfprintf(file, fmt, args);
        va_end(args);
        std::fputc('\n', file);
        std::fclose(file);
    }

    std::string GetDiagnosticComputerName()
    {
        char name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        if (GetComputerNameA(name, &length) && length > 0)
            return std::string(name, length);
        return "UnknownPC";
    }

    std::string SanitizePayloadForLog(std::string_view value)
    {
        std::string result(value);
        for (auto& c : result)
        {
            if (c == '\r' || c == '\n' || c == '\0')
                c = ' ';
        }
        return result;
    }

    bool IsPeerDiagnosticPayload(std::string_view payload)
    {
        if (!payload.starts_with("STRPM_E2E_V1|"))
            return false;

        const std::string localPcField = "|pc=" + g_computerName + "|";
        return payload.find(localPcField) == std::string_view::npos;
    }

    bool IsSTRSessionConnected()
    {
        const auto bridge = GetModuleHandleW(L"STRPluginMessagingBridge.dll");
        if (!bridge)
            return false;
        const auto raw = GetProcAddress(bridge, "STRPM_IsSTRSessionConnected");
        if (!raw)
            return false;
        return reinterpret_cast<IsSTRSessionConnectedFn>(raw)();
    }

    void STRPM_CALL ReceiveDiagnostic(
        const STRPM::Message* message,
        void*)
    {
        if (!message)
            return;

        const auto senderName = message->sender.displayName != nullptr ?
            message->sender.displayName : "<unknown>";
        const std::string_view payloadView(
            message->data != nullptr ? static_cast<const char*>(message->data) : "",
            message->size);
        const auto payload = SanitizePayloadForLog(payloadView);

        Log(
            "E2E RECEIVE channel=%s senderId=%llu senderName='%s' seq=%llu flags=0x%X bytes=%zu payload='%s'",
            message->channel != nullptr ? message->channel : "<null>",
            static_cast<unsigned long long>(message->sender.connectionID),
            senderName,
            static_cast<unsigned long long>(message->sequence),
            message->flags,
            message->size,
            payload.c_str());

        if (!IsPeerDiagnosticPayload(payloadView))
            return;

        g_peerConnectionID.store(message->sender.connectionID);
        if (!g_peerObserved.exchange(true))
        {
            Log(
                "E2E PEER OBSERVED senderId=%llu senderName='%s' payload='%s'",
                static_cast<unsigned long long>(message->sender.connectionID),
                senderName,
                payload.c_str());
        }

        if (payloadView.starts_with("STRPM_E2E_V1|ack=1|"))
        {
            if (!g_peerAckObserved.exchange(true))
            {
                Log(
                    "E2E PEER ACK OBSERVED senderId=%llu senderName='%s'",
                    static_cast<unsigned long long>(message->sender.connectionID),
                    senderName);
            }
        }
    }

    bool SleepInterruptible(
        std::stop_token token,
        std::chrono::milliseconds duration)
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

    STRPM::Result SendDiagnosticPayload(
        const STRPM::Interface* api,
        const STRPM::Target& target,
        std::string_view payload)
    {
        return api->send(
            kDiagnosticChannel,
            target,
            payload.data(),
            payload.size(),
            kProbeFlags);
    }

    bool ResolvePeerProxy(
        std::stop_token token,
        const STRPM::ProxyResolverInterface* resolver)
    {
        if (!resolver)
        {
            Log("PROXY RESOLVE unavailable: public ProxyResolver interface not loaded");
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + kProxyResolveTimeout;
        while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline)
        {
            const auto peer = g_peerConnectionID.load();
            if (peer != 0)
            {
                STRPM::ProxyFormID formID = STRPM::kInvalidProxyFormID;
                const auto result = resolver->resolve(peer, &formID);
                if (result == STRPM::Result::kOk && formID != STRPM::kInvalidProxyFormID)
                {
                    Log(
                        "PROXY RESOLVE OK senderId=%llu formId=0x%08X",
                        static_cast<unsigned long long>(peer),
                        static_cast<unsigned>(formID));
                    return true;
                }
            }
            if (!SleepInterruptible(token, kProxyPollDelay))
                return false;
        }

        Log(
            "PROXY RESOLVE TIMEOUT senderId=%llu (handshake transport is healthy; inspect bridge ProxyResolver lifecycle logs)",
            static_cast<unsigned long long>(g_peerConnectionID.load()));
        return false;
    }

    void DiagnosticWorker(std::stop_token token)
    {
        Log("diagnostic worker started; waiting for SKSE to load STRPluginMessagingAPI.dll");

        while (!token.stop_requested() &&
               GetModuleHandleW(L"STRPluginMessagingAPI.dll") == nullptr)
        {
            if (!SleepInterruptible(token, kApiRetryDelay))
                return;
        }

        const STRPM::Interface* api = nullptr;
        while (!token.stop_requested())
        {
            api = STRPM::LoadFromModule(L"STRPluginMessagingAPI.dll");
            if (api != nullptr)
                break;
            if (!SleepInterruptible(token, kApiRetryDelay))
                return;
        }

        if (!api)
            return;

        const auto* proxyResolver = STRPM::LoadProxyResolverFromModule(
            L"STRPluginMessagingAPI.dll");
        Log("public API v%u loaded; ProxyResolver=%s",
            api->version,
            proxyResolver ? "available" : "unavailable");
        g_computerName = GetDiagnosticComputerName();

        const auto registerResult = api->registerChannel(
            kDiagnosticChannel,
            &ReceiveDiagnostic,
            nullptr,
            &g_listener);
        if (registerResult != STRPM::Result::kOk)
        {
            Log(
                "failed to register channel '%s': %s",
                kDiagnosticChannel,
                STRPM::ResultToString(registerResult));
            return;
        }
        Log("registered public callback on channel '%s' for pc='%s'", kDiagnosticChannel, g_computerName.c_str());

        Log("E2E waiting for authoritative STR OnConnected lifecycle event before sending probes");
        while (!token.stop_requested() && !IsSTRSessionConnected())
        {
            if (!SleepInterruptible(token, kConnectionPollDelay))
                return;
        }
        if (!SleepInterruptible(token, kConnectionSettleDelay))
            return;
        Log("E2E STR connection observed; diagnostic sends enabled");

        STRPM::Target target{};
        target.kind = STRPM::TargetKind::kAllPlayers;

        auto lastWaitingLog = std::chrono::steady_clock::time_point{};
        std::uint32_t probeNumber = 1;
        while (!token.stop_requested() && probeNumber <= 2)
        {
            if (!IsSTRSessionConnected())
            {
                Log("E2E paused: STR disconnected while probes were pending");
                while (!token.stop_requested() && !IsSTRSessionConnected())
                {
                    if (!SleepInterruptible(token, kConnectionPollDelay))
                        return;
                }
                if (!SleepInterruptible(token, kConnectionSettleDelay))
                    return;
            }

            const auto payload =
                std::string("STRPM_E2E_V1|probe=") + std::to_string(probeNumber) +
                "|pc=" + g_computerName +
                "|pid=" + std::to_string(GetCurrentProcessId());

            const auto result = SendDiagnosticPayload(api, target, payload);
            if (result == STRPM::Result::kOk)
            {
                Log(
                    "E2E SEND OK probe=%u target=all flags=0x%X bytes=%zu payload='%s'",
                    probeNumber,
                    kProbeFlags,
                    payload.size(),
                    payload.c_str());
                ++probeNumber;
                if (probeNumber <= 2 && !SleepInterruptible(token, kProbeSpacing))
                    return;
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (lastWaitingLog.time_since_epoch().count() == 0 ||
                now - lastWaitingLog >= kSendRetryLogInterval)
            {
                Log("E2E send waiting after connected event: %s", STRPM::ResultToString(result));
                lastWaitingLog = now;
            }

            if (!SleepInterruptible(token, kSendRetryDelay))
                return;
        }

        Log("E2E initial probes complete; waiting for a peer probe before sending ACK");

        bool ackSent = false;
        auto lastAckWaitingLog = std::chrono::steady_clock::time_point{};
        while (!token.stop_requested())
        {
            if (g_peerObserved.load() && !ackSent)
            {
                const auto payload =
                    std::string("STRPM_E2E_V1|ack=1|pc=") + g_computerName +
                    "|pid=" + std::to_string(GetCurrentProcessId());
                const auto result = SendDiagnosticPayload(api, target, payload);
                if (result == STRPM::Result::kOk)
                {
                    Log(
                        "E2E ACK SEND OK target=all flags=0x%X bytes=%zu payload='%s'",
                        kProbeFlags,
                        payload.size(),
                        payload.c_str());
                    ackSent = true;
                }
                else
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (lastAckWaitingLog.time_since_epoch().count() == 0 ||
                        now - lastAckWaitingLog >= kSendRetryLogInterval)
                    {
                        Log("E2E ACK waiting: %s", STRPM::ResultToString(result));
                        lastAckWaitingLog = now;
                    }
                }
            }

            if (ackSent && g_peerAckObserved.load())
            {
                Log("E2E BIDIRECTIONAL HANDSHAKE COMPLETE");
                ResolvePeerProxy(token, proxyResolver);
                return;
            }

            if (!SleepInterruptible(token, kHandshakePollDelay))
                return;
        }
    }
}

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_8_1,
    "STRPluginMessagingDiagnostic",
    "Caelvanost",
    "",
    0,
    0,
    { STRPMSKSE::kRuntime_1_6_1170, 0 },
    0
};

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface*)
{
    {
        FILE* file = nullptr;
        fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingDiagnostic.log", "w");
        if (file)
        {
            std::fprintf(file, "STRPluginMessagingDiagnostic v0.8.1: SKSEPlugin_Load entered\n");
            std::fclose(file);
        }
    }

    g_worker = std::jthread(&DiagnosticWorker);
    return true;
}