#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"
#include "SKSEPluginVersionCompat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

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
    constexpr auto kSendRetryDelay = std::chrono::seconds(1);
    constexpr auto kSendRetryLogInterval = std::chrono::seconds(5);
    constexpr auto kProbeSpacing = std::chrono::seconds(5);
    constexpr std::uint32_t kProbeFlags =
        STRPM::kMessageReliable |
        STRPM::kMessageOrdered |
        STRPM::kMessageAllowLoopback;

    std::mutex g_logMutex;
    std::jthread g_worker;
    STRPM::ListenerHandle g_listener{};

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

        Log("public API v%u loaded", api->version);

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
        Log("registered public callback on channel '%s'", kDiagnosticChannel);

        const auto computerName = GetDiagnosticComputerName();
        STRPM::Target target{};
        target.kind = STRPM::TargetKind::kAllPlayers;

        auto lastWaitingLog = std::chrono::steady_clock::time_point{};
        std::uint32_t probeNumber = 1;
        while (!token.stop_requested() && probeNumber <= 2)
        {
            const auto payload =
                std::string("STRPM_E2E_V1|probe=") + std::to_string(probeNumber) +
                "|pc=" + computerName +
                "|pid=" + std::to_string(GetCurrentProcessId());

            const auto result = api->send(
                kDiagnosticChannel,
                target,
                payload.data(),
                payload.size(),
                kProbeFlags);

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
                Log(
                    "E2E send waiting: %s (connect both clients with F2, then send one normal STR chat message on this client)",
                    STRPM::ResultToString(result));
                lastWaitingLog = now;
            }

            if (!SleepInterruptible(token, kSendRetryDelay))
                return;
        }

        Log("E2E diagnostic send sequence complete; callback remains registered for peer probes");
    }
}

extern "C" __declspec(dllexport) STRPMSKSE::PluginVersionData SKSEPlugin_Version =
{
    STRPMSKSE::PluginVersionData::kVersion,
    STRPMSKSE::kPluginVersion_0_5_0,
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
            std::fprintf(file, "STRPluginMessagingDiagnostic v0.5.0: SKSEPlugin_Load entered\n");
            std::fclose(file);
        }
    }

    g_worker = std::jthread(&DiagnosticWorker);
    return true;
}
