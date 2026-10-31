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
    inline constexpr std::uint32_t kInterfaceVersion = 2;
    inline constexpr std::uint32_t kDiagnosticsVersion = 2;
    inline constexpr std::uint32_t kTransportInterfaceVersion = 1;
    inline constexpr std::uint32_t kProxyResolverVersion = 1;
    inline constexpr std::uint32_t kInventoryInterfaceVersion = 1;
    inline constexpr std::uint32_t kMaxInventoryEffects = 32;
    inline constexpr std::uint32_t kMaxChannelLength = 96;
    inline constexpr std::uint32_t kMaxPayloadBytes = 24 * 1024;
    inline constexpr char kQueryInterfaceExportName[] =
        "STR_QueryPluginMessagingInterface";
    inline constexpr char kQueryDiagnosticsExportName[] =
        "STR_QueryPluginMessagingDiagnostics";
    inline constexpr char kQueryTransportExportName[] =
        "STRPM_QueryTransportInterface";
    inline constexpr char kQueryProxyResolverExportName[] =
        "STR_QueryPluginMessagingProxyResolver";
    inline constexpr char kQueryInventoryExportName[] =
        "STR_QueryPluginMessagingInventory";

    using ConnectionID = std::uint64_t;
    using ProxyFormID = std::uint32_t;
    inline constexpr ProxyFormID kInvalidProxyFormID = 0;

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
        kTransportError = 9,
        kTargetNotFound = 10,
        kBufferTooSmall = 11,
        kBusy = 12,
        kUnsupportedItem = 13
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

    enum class RuntimeBackend : std::uint32_t
    {
        kNone = 0,
        kUdp = 1,
        kStrBridge = 2
    };

    enum class RuntimeBackendMode : std::uint32_t
    {
        kAuto = 0,
        kUdp = 1,
        kStrBridge = 2
    };

    enum class ProxyMappingEventType : std::uint32_t
    {
        kAdded = 1,
        kUpdated = 2,
        kRemoved = 3,
        kCleared = 4
    };

    struct Target
    {
        TargetKind kind{ TargetKind::kAllPlayers };
        ConnectionID connectionID{ 0 };
        const char* displayName{ nullptr };
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

    struct ProxyMappingEvent
    {
        ProxyMappingEventType type{ ProxyMappingEventType::kAdded };
        ConnectionID connectionID{ 0 };
        ProxyFormID oldFormID{ kInvalidProxyFormID };
        ProxyFormID newFormID{ kInvalidProxyFormID };
    };

    using ReceiveCallback = void(STRPM_CALL*)(
        const Message* message,
        void* userData);

    using LogCallback = void(STRPM_CALL*)(
        const char* message,
        void* userData);

    using ProxyMappingCallback = void(STRPM_CALL*)(
        const ProxyMappingEvent* event,
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

        Result(STRPM_CALL* setLocalDisplayName)(
            const char* displayName);
    };

    struct RuntimeStatus
    {
        std::uint32_t version{ kDiagnosticsVersion };
        std::uint32_t knownPeerCount{ 0 };
        std::uint32_t configuredPeerCount{ 0 };
        std::uint32_t autoDiscovery{ 0 };
        std::uint32_t relayMode{ 0 };
        std::uint32_t requireKnownPeer{ 0 };
        std::uint16_t localPort{ 0 };
        std::uint16_t reserved{ 0 };
        RuntimeBackend activeBackend{ RuntimeBackend::kNone };
        RuntimeBackendMode configuredBackendMode{ RuntimeBackendMode::kAuto };
        std::uint32_t strBridgeAvailable{ 0 };
        std::uint32_t strBridgeActive{ 0 };
    };

    struct DiagnosticsInterface
    {
        std::uint32_t version{ kDiagnosticsVersion };

        Result(STRPM_CALL* getRuntimeStatus)(
            RuntimeStatus* outStatus);
    };

    struct TransportInterface
    {
        std::uint32_t version{ kTransportInterfaceVersion };

        Result(STRPM_CALL* start)(
            ReceiveCallback callback,
            void* userData);

        Result(STRPM_CALL* stop)();

        Result(STRPM_CALL* send)(
            const char* channel,
            Target target,
            const void* data,
            std::size_t size,
            std::uint32_t flags);

        Result(STRPM_CALL* getLocalConnectionID)(
            ConnectionID* outConnectionID);

        Result(STRPM_CALL* setLocalDisplayName)(
            const char* displayName);
    };

    struct InventoryGameId
    {
        std::uint32_t baseId{ 0 };
        std::uint32_t modId{ 0 };
    };

    struct InventoryEffect
    {
        float magnitude{ 0.0f };
        std::int32_t area{ 0 };
        std::int32_t duration{ 0 };
        float rawCost{ 0.0f };
        InventoryGameId effectId{};
    };

    enum InventoryEntryFlags : std::uint32_t
    {
        kInventoryEntryNone = 0,
        kInventoryEntryEnchantIsWeapon = 1u << 0,
        kInventoryEntryEnchantRemoveOnUnequip = 1u << 1,
        kInventoryEntryWorn = 1u << 2,
        kInventoryEntryWornLeft = 1u << 3,
        kInventoryEntryQuestItem = 1u << 4
    };

    struct InventoryEntry
    {
        InventoryGameId baseId{};
        std::int32_t count{ 0 };

        float extraCharge{ 0.0f };

        InventoryGameId extraEnchantId{};
        std::uint16_t extraEnchantCharge{ 0 };
        std::uint16_t enchantEffectCount{ 0 };
        InventoryEffect enchantEffects[kMaxInventoryEffects]{};

        float extraHealth{ 0.0f };

        InventoryGameId extraPoisonId{};
        std::uint32_t extraPoisonCount{ 0 };

        std::int32_t extraSoulLevel{ 0 };
        std::uint32_t flags{ kInventoryEntryNone };
    };

    // localBaseFormID is only meaningful on the current client and is never
    // transmitted. entry is the portable STR Inventory::Entry representation.
    struct InventoryItem
    {
        std::uint32_t localBaseFormID{ 0 };
        InventoryEntry entry{};
    };

    using InventoryTransferID = std::uint64_t;

    enum class InventoryTransferState : std::uint32_t
    {
        kPrepared = 1,
        kRemovingLocal = 2,
        kCommitting = 3,
        kApplyingRemote = 4,
        kComplete = 5,
        kFailed = 6,
        kRolledBack = 7
    };

    struct InventoryTransferEvent
    {
        InventoryTransferID transferID{ 0 };
        ConnectionID peerConnectionID{ 0 };
        InventoryTransferState state{ InventoryTransferState::kPrepared };
        Result result{ Result::kOk };
        InventoryEntry entry{};
    };

    using InventoryTransferCallback = void(STRPM_CALL*)(
        const InventoryTransferEvent* event,
        void* userData);

    struct InventoryInterface
    {
        std::uint32_t version{ kInventoryInterfaceVersion };

        // Two-call pattern: call with items=nullptr/capacity=0 to obtain the
        // required item count, then call again with a sufficiently large buffer.
        Result(STRPM_CALL* getLocalInventory)(
            InventoryItem* items,
            std::size_t capacity,
            std::size_t* inOutCount);

        // Starts an asynchronous owner-to-owner transfer. The sender removes the
        // item locally through STR's native Inventory::Entry path; the recipient
        // then applies the same canonical entry locally. Consumers never handle
        // Skyrim ExtraDataList objects.
        Result(STRPM_CALL* transfer)(
            ConnectionID recipientConnectionID,
            const InventoryEntry* entry,
            InventoryTransferID* outTransferID);

        Result(STRPM_CALL* registerTransferListener)(
            InventoryTransferCallback callback,
            void* userData);

        Result(STRPM_CALL* unregisterTransferListener)(
            InventoryTransferCallback callback,
            void* userData);
    };

    struct ProxyResolverInterface
    {
        std::uint32_t version{ kProxyResolverVersion };

        Result(STRPM_CALL* resolve)(
            ConnectionID connectionID,
            ProxyFormID* outFormID);

        Result(STRPM_CALL* registerListener)(
            ProxyMappingCallback callback,
            void* userData);

        Result(STRPM_CALL* unregisterListener)(
            ProxyMappingCallback callback,
            void* userData);
    };

    using QueryInterfaceFn = Result(STRPM_CALL*)(
        std::uint32_t requestedVersion,
        const Interface** outInterface);

    using QueryDiagnosticsFn = Result(STRPM_CALL*)(
        std::uint32_t requestedVersion,
        const DiagnosticsInterface** outInterface);

    using QueryTransportInterfaceFn = Result(STRPM_CALL*)(
        std::uint32_t requestedVersion,
        const TransportInterface** outInterface);

    using QueryProxyResolverFn = Result(STRPM_CALL*)(
        std::uint32_t requestedVersion,
        const ProxyResolverInterface** outInterface);

    using QueryInventoryInterfaceFn = Result(STRPM_CALL*)(
        std::uint32_t requestedVersion,
        const InventoryInterface** outInterface);

    [[nodiscard]] const Interface* LoadFromModule(
        const wchar_t* moduleName = L"STRPluginMessagingAPI.dll") noexcept;

    [[nodiscard]] const DiagnosticsInterface* LoadDiagnosticsFromModule(
        const wchar_t* moduleName = L"STRPluginMessagingAPI.dll") noexcept;

    [[nodiscard]] const TransportInterface* LoadTransportFromModule(
        const wchar_t* moduleName) noexcept;

    [[nodiscard]] const ProxyResolverInterface* LoadProxyResolverFromModule(
        const wchar_t* moduleName = L"STRPluginMessagingAPI.dll") noexcept;

    [[nodiscard]] const InventoryInterface* LoadInventoryFromModule(
        const wchar_t* moduleName = L"STRPluginMessagingBridge.dll") noexcept;

    [[nodiscard]] const char* ResultToString(Result result) noexcept;
}
