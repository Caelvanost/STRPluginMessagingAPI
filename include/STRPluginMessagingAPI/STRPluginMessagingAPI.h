#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define STRPM_CALL __cdecl
#define STRPM_EXPORT extern "C" __declspec(dllexport)
#else
#define STRPM_CALL
#define STRPM_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace STRPM
{
    inline constexpr std::uint32_t kInterfaceVersion = 1;
    inline constexpr std::uint32_t kMaxChannelLength = 96;
    inline constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024;
    inline constexpr char kQueryInterfaceExportName[] =
        "STR_QueryPluginMessagingInterface";

    using ConnectionID = std::uint64_t;

    enum class Result : std::uint32_t
    {
        kOk = 0,
        kNotAvailable = 1,
        kUnsupportedVersion = 2,
        kInvalidArgument = 3,
        kNotConnected = 4,
        kChannelAlreadyRegistered = 5,
        kChannelNotRegistered = 6,
        kPayloadTooLarge = 7,
        kRateLimited = 8,
        kTransportError = 9
    };

    enum MessageFlags : std::uint32_t
    {
        kMessageNone = 0,
        kMessageReliable = 1u << 0,
        kMessageOrdered = 1u << 1,
        kMessageAllowLoopback = 1u << 2
    };

    enum class TargetKind : std::uint32_t
    {
        kServer = 1,
        kHost = 2,
        kPlayer = 3,
        kAllPlayers = 4
    };

    struct Target
    {
        TargetKind kind{ TargetKind::kAllPlayers };
        ConnectionID connectionID{ 0 };
    };

    struct Sender
    {
        ConnectionID connectionID{ 0 };
        const char* displayName{ nullptr };
        bool isHost{ false };
    };

    struct Message
    {
        const char* channel{ nullptr };
        const void* data{ nullptr };
        std::size_t size{ 0 };
        Sender sender{};
        std::uint32_t flags{ kMessageNone };
        std::uint64_t sequence{ 0 };
    };

    struct ListenerHandle
    {
        std::uint64_t value{ 0 };
    };

    using ReceiveCallback = void(STRPM_CALL*)(
        const Message* message,
        void* userData);

    using LogCallback = void(STRPM_CALL*)(
        const char* message,
        void* userData);

    struct Interface
    {
        std::uint32_t version{ kInterfaceVersion };

        Result(STRPM_CALL* registerChannel)(
            const char* channel,
            ReceiveCallback callback,
            void* userData,
            ListenerHandle* outHandle);

        Result(STRPM_CALL* unregisterChannel)(
            ListenerHandle handle);

        Result(STRPM_CALL* send)(
            const char* channel,
            Target target,
            const void* data,
            std::size_t size,
            std::uint32_t flags);

        Result(STRPM_CALL* getLocalConnectionID)(
            ConnectionID* outConnectionID);

        Result(STRPM_CALL* setLogCallback)(
            LogCallback callback,
            void* userData);
    };

    using QueryInterfaceFn = Result(STRPM_CALL*)(
        std::uint32_t requestedVersion,
        const Interface** outInterface);

    [[nodiscard]] const Interface* LoadFromModule(
        const wchar_t* moduleName = L"SkyrimTogetherReborn.dll") noexcept;

    [[nodiscard]] const char* ResultToString(Result result) noexcept;
}
