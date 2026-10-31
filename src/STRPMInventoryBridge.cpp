#include "STRPMInventoryBridge.h"

#include "STRPMInventoryNative.h"
#include "STRPMProxyResolverBridge.h"
#include "STRPMServerModMap.h"
#include "STRPMSTRCompatibility.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace STRPMInventoryBridge
{
    namespace
    {
        using namespace std::chrono_literals;
        namespace Native = STRPMInventoryNative;

        constexpr char kInventoryChannel[] = "strpm.inventory.v1";
        constexpr std::uint32_t kWireMagic = 0x31564E49u; // "INV1"
        constexpr std::uint16_t kWireVersion = 1;
        constexpr std::size_t kChunkBytes = 1u * 1024u * 1024u;
        constexpr auto kWorkerPoll = 250ms;
        constexpr auto kPrepareTimeout = 10s;
        constexpr auto kCommitRetryInterval = 2s;
        constexpr std::uint32_t kMaxCommitAttempts = 5;
        constexpr auto kIncomingRetention = 2min;

        constexpr std::string_view kGetItemFromExtraDataAnchor =
            "Null enchantment in ExtraEnchantment";
        constexpr std::string_view kAddOrRemoveItemAnchor =
            "{}: Object to add not found, {:X}:{:X}.";

        enum class WireType : std::uint16_t
        {
            kPrepare = 1,
            kReady = 2,
            kCommit = 3,
            kApplied = 4,
            kAbort = 5
        };

        struct WireMessage
        {
            std::uint32_t magic{ kWireMagic };
            std::uint16_t version{ kWireVersion };
            WireType type{ WireType::kPrepare };
            std::uint32_t result{ static_cast<std::uint32_t>(STRPM::Result::kOk) };
            STRPM::InventoryTransferID transferID{ 0 };
            STRPM::InventoryEntry entry{};
        };
        static_assert(std::is_trivially_copyable_v<STRPM::InventoryEntry>);
        static_assert(std::is_trivially_copyable_v<WireMessage>);

        struct MemorySpan
        {
            std::uintptr_t base{ 0 };
            std::size_t size{ 0 };
            bool readable{ false };
            bool executable{ false };
        };

        struct FunctionBounds
        {
            std::uintptr_t begin{ 0 };
            std::uintptr_t end{ 0 };
        };

        struct Listener
        {
            STRPM::InventoryTransferCallback callback{ nullptr };
            void* userData{ nullptr };
        };

        enum class OutgoingPhase
        {
            kAwaitReady,
            kRemoving,
            kAwaitApplied
        };

        struct OutgoingTransaction
        {
            STRPM::ConnectionID peer{ 0 };
            STRPM::InventoryEntry entry{};
            OutgoingPhase phase{ OutgoingPhase::kAwaitReady };
            std::uint32_t commitAttempts{ 0 };
            std::chrono::steady_clock::time_point lastAction{};
        };

        struct TransactionKey
        {
            STRPM::ConnectionID sender{ 0 };
            STRPM::InventoryTransferID transferID{ 0 };

            bool operator==(const TransactionKey& rhs) const noexcept
            {
                return sender == rhs.sender && transferID == rhs.transferID;
            }
        };

        struct TransactionKeyHash
        {
            std::size_t operator()(const TransactionKey& value) const noexcept
            {
                const auto a = std::hash<STRPM::ConnectionID>{}(value.sender);
                const auto b =
                    std::hash<STRPM::InventoryTransferID>{}(value.transferID);
                return a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
            }
        };

        struct IncomingTransaction
        {
            STRPM::InventoryEntry entry{};
            bool validationScheduled{ false };
            bool ready{ false };
            bool applyScheduled{ false };
            bool applied{ false };
            std::chrono::steady_clock::time_point lastAction{};
        };

        std::atomic_bool g_started{ false };
        std::atomic_bool g_ready{ false };
        std::atomic<std::uintptr_t> g_getItemFromExtraDataAddress{ 0 };
        std::atomic<std::uintptr_t> g_addOrRemoveItemAddress{ 0 };
        std::atomic<STRPM::InventoryTransferID> g_nextTransferID{
            static_cast<STRPM::InventoryTransferID>(GetTickCount64()) << 20
        };

        std::jthread g_worker;
        STRPM::ListenerHandle g_inventoryChannelHandle{};
        std::atomic_bool g_channelRegistered{ false };

        std::mutex g_listenerMutex;
        std::vector<Listener> g_listeners;

        std::mutex g_transactionMutex;
        std::unordered_map<STRPM::InventoryTransferID, OutgoingTransaction>
            g_outgoing;
        std::unordered_map<TransactionKey, IncomingTransaction, TransactionKeyHash>
            g_incoming;

        void Log(const char* format, ...) noexcept
        {
            FILE* file = nullptr;
            fopen_s(
                &file,
                "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log",
                "a");
            if (!file)
                return;

            va_list args;
            va_start(args, format);
            std::vfprintf(file, format, args);
            va_end(args);
            std::fputc('\n', file);
            std::fclose(file);
        }

        bool IsReadableProtection(DWORD protect) noexcept
        {
            if ((protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS)
                return false;
            const DWORD value = protect & 0xFF;
            return value == PAGE_READONLY || value == PAGE_READWRITE ||
                   value == PAGE_WRITECOPY || value == PAGE_EXECUTE_READ ||
                   value == PAGE_EXECUTE_READWRITE ||
                   value == PAGE_EXECUTE_WRITECOPY;
        }

        bool IsExecutableProtection(DWORD protect) noexcept
        {
            if ((protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS)
                return false;
            const DWORD value = protect & 0xFF;
            return value == PAGE_EXECUTE || value == PAGE_EXECUTE_READ ||
                   value == PAGE_EXECUTE_READWRITE ||
                   value == PAGE_EXECUTE_WRITECOPY;
        }

        bool SnapshotProcessMemory(
            std::uintptr_t address,
            std::size_t size,
            std::vector<std::uint8_t>& snapshot) noexcept
        {
            snapshot.clear();
            if (address == 0 || size == 0 || size > kChunkBytes + 256)
                return false;

            try
            {
                snapshot.resize(size);
            }
            catch (...)
            {
                snapshot.clear();
                return false;
            }

            SIZE_T bytesRead = 0;
            if (!ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(address),
                    snapshot.data(),
                    size,
                    &bytesRead) ||
                bytesRead != size)
            {
                snapshot.clear();
                return false;
            }
            return true;
        }

        bool IsExecutableAddress(
            std::uintptr_t address,
            HMODULE module) noexcept
        {
            MEMORY_BASIC_INFORMATION mbi{};
            return address != 0 &&
                   VirtualQuery(
                       reinterpret_cast<void*>(address),
                       &mbi,
                       sizeof(mbi)) == sizeof(mbi) &&
                   mbi.State == MEM_COMMIT &&
                   IsExecutableProtection(mbi.Protect) &&
                   (!module || mbi.AllocationBase == module);
        }

        std::vector<MemorySpan> EnumerateRuntimeMemory(HMODULE module)
        {
            std::vector<MemorySpan> result;
            if (!module)
                return result;

            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            auto current =
                reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
            const auto maximum =
                reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

            while (current < maximum)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(
                        reinterpret_cast<void*>(current),
                        &mbi,
                        sizeof(mbi)) != sizeof(mbi))
                {
                    break;
                }

                const auto base =
                    reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                const auto size = static_cast<std::size_t>(mbi.RegionSize);
                if (mbi.State == MEM_COMMIT &&
                    mbi.AllocationBase == module &&
                    size != 0)
                {
                    const bool readable = IsReadableProtection(mbi.Protect);
                    const bool executable = IsExecutableProtection(mbi.Protect);
                    if (readable || executable)
                        result.push_back(
                            { base, size, readable, executable });
                }

                if (size == 0 || base + size <= current)
                    break;
                current = base + size;
            }
            return result;
        }

        std::vector<std::uintptr_t> FindBytes(
            const std::vector<MemorySpan>& spans,
            std::string_view needle)
        {
            std::vector<std::uintptr_t> result;
            if (needle.empty())
                return result;

            const auto overlap = needle.size() - 1;
            for (const auto& span : spans)
            {
                if (!span.readable || span.size < needle.size())
                    continue;

                for (std::size_t offset = 0;
                     offset < span.size;
                     offset += kChunkBytes)
                {
                    const auto remaining = span.size - offset;
                    const auto payload = std::min(kChunkBytes, remaining);
                    const auto readSize =
                        std::min(remaining, payload + overlap);

                    std::vector<std::uint8_t> snapshot;
                    if (!SnapshotProcessMemory(
                            span.base + offset,
                            readSize,
                            snapshot))
                    {
                        continue;
                    }

                    for (std::size_t i = 0;
                         i < payload &&
                         i + needle.size() <= snapshot.size();
                         ++i)
                    {
                        if (std::memcmp(
                                snapshot.data() + i,
                                needle.data(),
                                needle.size()) == 0)
                        {
                            result.push_back(span.base + offset + i);
                        }
                    }
                }
            }

            std::sort(result.begin(), result.end());
            result.erase(
                std::unique(result.begin(), result.end()),
                result.end());
            return result;
        }

        std::vector<std::uintptr_t> FindRipRelativeXrefs(
            const std::vector<MemorySpan>& spans,
            const std::vector<std::uintptr_t>& targets)
        {
            std::vector<std::uintptr_t> result;
            if (targets.empty())
                return result;

            constexpr std::size_t kInstructionBytes = 7;
            constexpr std::size_t kOverlap = kInstructionBytes - 1;

            for (const auto& span : spans)
            {
                if (!span.executable || span.size < kInstructionBytes)
                    continue;

                for (std::size_t offset = 0;
                     offset < span.size;
                     offset += kChunkBytes)
                {
                    const auto remaining = span.size - offset;
                    const auto payload = std::min(kChunkBytes, remaining);
                    const auto readSize =
                        std::min(remaining, payload + kOverlap);

                    std::vector<std::uint8_t> snapshot;
                    if (!SnapshotProcessMemory(
                            span.base + offset,
                            readSize,
                            snapshot))
                    {
                        continue;
                    }

                    const auto* code = snapshot.data();
                    for (std::size_t i = 0;
                         i < payload &&
                         i + kInstructionBytes <= snapshot.size();
                         ++i)
                    {
                        if ((code[i] & 0xF8) != 0x48 ||
                            code[i + 1] != 0x8D)
                        {
                            continue;
                        }

                        const auto modrm = code[i + 2];
                        if ((modrm & 0xC7) != 0x05)
                            continue;

                        std::int32_t displacement = 0;
                        std::memcpy(
                            &displacement,
                            code + i + 3,
                            sizeof(displacement));

                        const auto instruction = span.base + offset + i;
                        const auto target =
                            instruction + 7 +
                            static_cast<std::intptr_t>(displacement);
                        if (std::find(
                                targets.begin(),
                                targets.end(),
                                target) != targets.end())
                        {
                            result.push_back(instruction);
                        }
                    }
                }
            }

            std::sort(result.begin(), result.end());
            result.erase(
                std::unique(result.begin(), result.end()),
                result.end());
            return result;
        }

        std::optional<FunctionBounds> GetFunctionBounds(
            std::uintptr_t address,
            HMODULE module) noexcept
        {
            DWORD64 imageBase = 0;
            auto* function = RtlLookupFunctionEntry(
                static_cast<DWORD64>(address),
                &imageBase,
                nullptr);
            if (!function ||
                imageBase != reinterpret_cast<DWORD64>(module))
            {
                return std::nullopt;
            }

            FunctionBounds bounds{
                static_cast<std::uintptr_t>(
                    imageBase + function->BeginAddress),
                static_cast<std::uintptr_t>(
                    imageBase + function->EndAddress)
            };
            if (bounds.begin == 0 ||
                bounds.end <= bounds.begin ||
                !IsExecutableAddress(bounds.begin, module))
            {
                return std::nullopt;
            }
            return bounds;
        }

        std::optional<FunctionBounds> FindFunctionByAnchor(
            const std::vector<MemorySpan>& spans,
            HMODULE module,
            std::string_view anchor)
        {
            const auto copies = FindBytes(spans, anchor);
            const auto xrefs = FindRipRelativeXrefs(spans, copies);

            std::vector<FunctionBounds> candidates;
            for (const auto xref : xrefs)
            {
                if (const auto bounds = GetFunctionBounds(xref, module))
                    candidates.push_back(*bounds);
            }

            std::sort(
                candidates.begin(),
                candidates.end(),
                [](const auto& left, const auto& right) {
                    return left.begin < right.begin ||
                           (left.begin == right.begin &&
                            left.end < right.end);
                });
            candidates.erase(
                std::unique(
                    candidates.begin(),
                    candidates.end(),
                    [](const auto& left, const auto& right) {
                        return left.begin == right.begin &&
                               left.end == right.end;
                    }),
                candidates.end());

            if (candidates.size() != 1)
            {
                Log(
                    "STRPM inventory resolver anchor '%.*s': copies=%zu xrefs=%zu functions=%zu",
                    static_cast<int>(anchor.size()),
                    anchor.data(),
                    copies.size(),
                    xrefs.size(),
                    candidates.size());
                return std::nullopt;
            }
            return candidates.front();
        }

        bool ResolveNativeFunctions() noexcept
        {
            if (g_getItemFromExtraDataAddress.load() != 0 &&
                g_addOrRemoveItemAddress.load() != 0)
            {
                return true;
            }

            const auto module = GetModuleHandleW(nullptr);
            if (!module)
                return false;

            try
            {
                const auto spans = EnumerateRuntimeMemory(module);
                const auto getItem = FindFunctionByAnchor(
                    spans,
                    module,
                    kGetItemFromExtraDataAnchor);
                const auto addOrRemove = FindFunctionByAnchor(
                    spans,
                    module,
                    kAddOrRemoveItemAnchor);

                if (!getItem || !addOrRemove)
                    return false;

                g_getItemFromExtraDataAddress.store(getItem->begin);
                g_addOrRemoveItemAddress.store(addOrRemove->begin);

                Log(
                    "STRPM inventory native bridge resolved: GetItemFromExtraData=0x%llX AddOrRemoveItem=0x%llX",
                    static_cast<unsigned long long>(getItem->begin),
                    static_cast<unsigned long long>(addOrRemove->begin));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool InvokeGetItemFromExtraData(
            Native::Entry* entry,
            RE::ExtraDataList* extraData) noexcept
        {
            const auto address =
                g_getItemFromExtraDataAddress.load();
            if (!entry || !extraData || address == 0)
                return false;

            using Fn = void(__fastcall*)(
                Native::Entry*,
                RE::ExtraDataList*);
            const auto function = reinterpret_cast<Fn>(address);

#if defined(_MSC_VER)
            __try
            {
                function(entry, extraData);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#else
            function(entry, extraData);
            return true;
#endif
        }

        bool InvokeAddOrRemoveItem(
            RE::TESObjectREFR* reference,
            const Native::Entry* entry) noexcept
        {
            const auto address = g_addOrRemoveItemAddress.load();
            if (!reference || !entry || address == 0)
                return false;

            using Fn = void(__fastcall*)(
                RE::TESObjectREFR*,
                const Native::Entry*,
                bool);
            const auto function = reinterpret_cast<Fn>(address);

#if defined(_MSC_VER)
            __try
            {
                function(reference, entry, false);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#else
            function(reference, entry, false);
            return true;
#endif
        }

        const STRPM::Interface* QueryMessagingApi() noexcept
        {
            const auto module =
                GetModuleHandleW(L"STRPluginMessagingAPI.dll");
            if (!module)
                return nullptr;

            const auto raw =
                GetProcAddress(module, STRPM::kQueryInterfaceExportName);
            if (!raw)
                return nullptr;

            const auto query =
                reinterpret_cast<STRPM::QueryInterfaceFn>(raw);
            const STRPM::Interface* api = nullptr;
            if (query(STRPM::kInterfaceVersion, &api) !=
                    STRPM::Result::kOk ||
                !api ||
                api->version != STRPM::kInterfaceVersion)
            {
                return nullptr;
            }
            return api;
        }

        bool FilenameEquals(
            std::string_view left,
            std::string_view right) noexcept
        {
            if (left.size() != right.size())
                return false;

            for (std::size_t i = 0; i < left.size(); ++i)
            {
                const auto a = static_cast<unsigned char>(left[i]);
                const auto b = static_cast<unsigned char>(right[i]);
                if (std::tolower(a) != std::tolower(b))
                    return false;
            }
            return true;
        }

        std::optional<STRPMServerModMap::Entry> FindServerModByFilename(
            std::string_view filename)
        {
            std::vector<STRPMServerModMap::Entry> mods;
            if (!STRPMServerModMap::Snapshot(mods))
                return std::nullopt;

            const auto it = std::find_if(
                mods.begin(),
                mods.end(),
                [&](const auto& mod) {
                    return FilenameEquals(mod.filename, filename);
                });
            if (it == mods.end())
                return std::nullopt;
            return *it;
        }

        std::optional<STRPMServerModMap::Entry> FindServerModById(
            std::uint32_t id)
        {
            if (id > 0xFFFFu)
                return std::nullopt;

            std::vector<STRPMServerModMap::Entry> mods;
            if (!STRPMServerModMap::Snapshot(mods))
                return std::nullopt;

            const auto it = std::find_if(
                mods.begin(),
                mods.end(),
                [&](const auto& mod) {
                    return mod.id == static_cast<std::uint16_t>(id);
                });
            if (it == mods.end())
                return std::nullopt;
            return *it;
        }

        bool CanonicalizeLocalFormID(
            std::uint32_t localFormID,
            STRPM::InventoryGameId& outGameId) noexcept
        {
            outGameId = {};
            if (localFormID == 0)
                return false;

            const auto high = localFormID >> 24;
            if (high == 0xFF)
            {
                outGameId.baseId = localFormID & 0x00FFFFFFu;
                outGameId.modId = 0xFFFFFFFFu;
                return true;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
                return false;

            const RE::TESFile* file = nullptr;
            std::uint32_t baseId = 0;
            if (high == 0xFE)
            {
                const auto lightIndex = static_cast<std::uint16_t>(
                    (localFormID & 0x00FFF000u) >> 12);
                file =
                    dataHandler->LookupLoadedLightModByIndex(lightIndex);
                baseId = localFormID & 0x00000FFFu;
            }
            else
            {
                file = dataHandler->LookupLoadedModByIndex(
                    static_cast<std::uint8_t>(high));
                baseId = localFormID & 0x00FFFFFFu;
            }

            if (!file)
                return false;

            const auto serverMod =
                FindServerModByFilename(file->GetFilename());
            if (!serverMod)
                return false;

            outGameId.baseId = baseId;
            outGameId.modId = serverMod->id;
            return true;
        }

        bool ResolveGameIdToLocalFormID(
            const STRPM::InventoryGameId& gameId,
            std::uint32_t& outFormID) noexcept
        {
            outFormID = 0;
            if (gameId.baseId == 0 && gameId.modId == 0)
                return false;

            // A temporary base form only exists on the source client. STR 1.8.4
            // does not serialize enough recipe data to reconstruct crafted
            // temporary base forms on another client.
            if (gameId.modId == 0xFFFFFFFFu)
                return false;

            const auto serverMod = FindServerModById(gameId.modId);
            if (!serverMod)
                return false;

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
                return false;

            const RE::TESFile* file = serverMod->isLite ?
                dataHandler->LookupLoadedLightModByName(
                    serverMod->filename) :
                dataHandler->LookupLoadedModByName(
                    serverMod->filename);
            if (!file)
                return false;

            if (serverMod->isLite)
            {
                outFormID =
                    0xFE000000u |
                    (static_cast<std::uint32_t>(
                        file->GetSmallFileCompileIndex()) << 12) |
                    (gameId.baseId & 0x00000FFFu);
            }
            else
            {
                outFormID =
                    (static_cast<std::uint32_t>(
                        file->GetCompileIndex()) << 24) |
                    (gameId.baseId & 0x00FFFFFFu);
            }

            return RE::TESForm::LookupByID(outFormID) != nullptr;
        }

        bool IsZeroGameId(
            const STRPM::InventoryGameId& value) noexcept
        {
            return value.baseId == 0 && value.modId == 0;
        }

        bool ValidateEntryStructure(
            const STRPM::InventoryEntry& entry) noexcept
        {
            if (entry.count <= 0 ||
                entry.enchantEffectCount >
                    STRPM::kMaxInventoryEffects)
            {
                return false;
            }

            if ((entry.baseId.baseId == 0 &&
                 entry.baseId.modId == 0) ||
                entry.baseId.modId == 0xFFFFFFFFu)
            {
                return false;
            }

            if ((entry.flags & STRPM::kInventoryEntryQuestItem) != 0 ||
                (entry.flags & STRPM::kInventoryEntryWorn) != 0 ||
                (entry.flags & STRPM::kInventoryEntryWornLeft) != 0)
            {
                // Trading equipped or quest-bound instances is deliberately
                // left to consumer policy / an explicit unequip step.
                return false;
            }

            if (!IsZeroGameId(entry.extraEnchantId) &&
                entry.extraEnchantId.modId == 0xFFFFFFFFu &&
                entry.enchantEffectCount == 0)
            {
                return false;
            }

            if (!IsZeroGameId(entry.extraPoisonId) &&
                entry.extraPoisonId.modId == 0xFFFFFFFFu)
            {
                return false;
            }

            return true;
        }

        bool ValidatePortableEntryOnGameThread(
            const STRPM::InventoryEntry& entry) noexcept
        {
            if (!ValidateEntryStructure(entry))
                return false;

            std::uint32_t ignored = 0;
            if (!ResolveGameIdToLocalFormID(entry.baseId, ignored))
                return false;

            if (!IsZeroGameId(entry.extraEnchantId) &&
                entry.extraEnchantId.modId != 0xFFFFFFFFu &&
                !ResolveGameIdToLocalFormID(
                    entry.extraEnchantId,
                    ignored))
            {
                return false;
            }

            if (!IsZeroGameId(entry.extraPoisonId) &&
                !ResolveGameIdToLocalFormID(
                    entry.extraPoisonId,
                    ignored))
            {
                return false;
            }

            for (std::size_t i = 0;
                 i < entry.enchantEffectCount;
                 ++i)
            {
                if (!ResolveGameIdToLocalFormID(
                        entry.enchantEffects[i].effectId,
                        ignored))
                {
                    return false;
                }
            }

            return true;
        }

        bool EntriesEquivalentIgnoringCount(
            const STRPM::InventoryEntry& left,
            const STRPM::InventoryEntry& right) noexcept
        {
            if (left.baseId.baseId != right.baseId.baseId ||
                left.baseId.modId != right.baseId.modId ||
                left.extraCharge != right.extraCharge ||
                left.extraEnchantId.baseId !=
                    right.extraEnchantId.baseId ||
                left.extraEnchantId.modId !=
                    right.extraEnchantId.modId ||
                left.extraEnchantCharge !=
                    right.extraEnchantCharge ||
                left.enchantEffectCount !=
                    right.enchantEffectCount ||
                left.extraHealth != right.extraHealth ||
                left.extraPoisonId.baseId !=
                    right.extraPoisonId.baseId ||
                left.extraPoisonId.modId !=
                    right.extraPoisonId.modId ||
                left.extraPoisonCount !=
                    right.extraPoisonCount ||
                left.extraSoulLevel != right.extraSoulLevel ||
                left.flags != right.flags)
            {
                return false;
            }

            for (std::size_t i = 0;
                 i < left.enchantEffectCount;
                 ++i)
            {
                const auto& a = left.enchantEffects[i];
                const auto& b = right.enchantEffects[i];
                if (a.magnitude != b.magnitude ||
                    a.area != b.area ||
                    a.duration != b.duration ||
                    a.rawCost != b.rawCost ||
                    a.effectId.baseId != b.effectId.baseId ||
                    a.effectId.modId != b.effectId.modId)
                {
                    return false;
                }
            }
            return true;
        }

        void MergeInventoryItem(
            std::vector<STRPM::InventoryItem>& items,
            STRPM::InventoryItem item)
        {
            const auto it = std::find_if(
                items.begin(),
                items.end(),
                [&](const auto& existing) {
                    return existing.localBaseFormID ==
                               item.localBaseFormID &&
                           EntriesEquivalentIgnoringCount(
                               existing.entry,
                               item.entry);
                });

            if (it != items.end())
                it->entry.count += item.entry.count;
            else
                items.push_back(std::move(item));
        }

        STRPM::Result BuildLocalInventory(
            std::vector<STRPM::InventoryItem>& outItems)
        {
            outItems.clear();
            if (!ResolveNativeFunctions())
                return STRPM::Result::kNotAvailable;

            std::vector<STRPMServerModMap::Entry> serverMods;
            if (!STRPMServerModMap::Snapshot(serverMods))
                return STRPM::Result::kNotConnected;

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player)
                return STRPM::Result::kNotAvailable;

            try
            {
                auto inventory = player->GetInventory();
                for (auto& [object, data] : inventory)
                {
                    if (!object || data.first <= 0)
                        continue;

                    STRPM::InventoryGameId canonicalBase{};
                    if (!CanonicalizeLocalFormID(
                            object->GetFormID(),
                            canonicalBase))
                    {
                        continue;
                    }

                    std::int32_t remaining = data.first;
                    auto* entryData = data.second.get();
                    if (entryData && entryData->extraLists)
                    {
                        for (auto* extraData : *entryData->extraLists)
                        {
                            if (!extraData)
                                continue;

                            Native::Entry nativeEntry{};
                            nativeEntry.BaseId =
                                Native::ToNative(canonicalBase);
                            nativeEntry.Count = 1;

                            if (!InvokeGetItemFromExtraData(
                                    &nativeEntry,
                                    extraData))
                            {
                                Log(
                                    "STRPM inventory snapshot: GetItemFromExtraData failed for formId=0x%08X",
                                    object->GetFormID());
                                continue;
                            }

                            if (nativeEntry.Count <= 0)
                                continue;

                            STRPM::InventoryItem item{};
                            item.localBaseFormID =
                                object->GetFormID();
                            const auto converted =
                                Native::ToPublic(
                                    nativeEntry,
                                    item.entry);
                            if (converted != STRPM::Result::kOk)
                            {
                                Log(
                                    "STRPM inventory snapshot skipped unsupported extra-data entry formId=0x%08X",
                                    object->GetFormID());
                                continue;
                            }

                            remaining -= nativeEntry.Count;
                            MergeInventoryItem(
                                outItems,
                                std::move(item));
                        }
                    }

                    if (remaining > 0)
                    {
                        STRPM::InventoryItem item{};
                        item.localBaseFormID =
                            object->GetFormID();
                        item.entry.baseId = canonicalBase;
                        item.entry.count = remaining;
                        MergeInventoryItem(
                            outItems,
                            std::move(item));
                    }
                }
            }
            catch (...)
            {
                outItems.clear();
                return STRPM::Result::kTransportError;
            }

            return STRPM::Result::kOk;
        }

        std::int32_t GetMatchingLocalCount(
            const STRPM::InventoryEntry& wanted)
        {
            std::vector<STRPM::InventoryItem> items;
            if (BuildLocalInventory(items) != STRPM::Result::kOk)
                return 0;

            std::int32_t count = 0;
            for (const auto& item : items)
            {
                if (EntriesEquivalentIgnoringCount(
                        item.entry,
                        wanted))
                {
                    count += item.entry.count;
                }
            }
            return count;
        }

        void NotifyTransferEvent(
            STRPM::InventoryTransferID transferID,
            STRPM::ConnectionID peer,
            STRPM::InventoryTransferState state,
            STRPM::Result result,
            const STRPM::InventoryEntry& entry)
        {
            std::vector<Listener> listeners;
            {
                std::scoped_lock lock(g_listenerMutex);
                listeners = g_listeners;
            }

            const STRPM::InventoryTransferEvent event{
                transferID,
                peer,
                state,
                result,
                entry
            };
            for (const auto& listener : listeners)
            {
                if (listener.callback)
                    listener.callback(
                        &event,
                        listener.userData);
            }
        }

        STRPM::Result SendWire(
            STRPM::ConnectionID peer,
            WireType type,
            STRPM::InventoryTransferID transferID,
            const STRPM::InventoryEntry& entry,
            STRPM::Result result = STRPM::Result::kOk)
        {
            const auto* api = QueryMessagingApi();
            if (!api || !api->send)
                return STRPM::Result::kNotAvailable;

            WireMessage wire{};
            wire.type = type;
            wire.result = static_cast<std::uint32_t>(result);
            wire.transferID = transferID;
            wire.entry = entry;

            STRPM::Target target{};
            target.kind = STRPM::TargetKind::kPlayer;
            target.connectionID = peer;

            return api->send(
                kInventoryChannel,
                target,
                &wire,
                sizeof(wire),
                STRPM::kMessageReliable |
                    STRPM::kMessageOrdered);
        }

        void FailOutgoing(
            STRPM::InventoryTransferID transferID,
            STRPM::Result result)
        {
            OutgoingTransaction transaction{};
            bool found = false;
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(transferID);
                if (it != g_outgoing.end())
                {
                    transaction = it->second;
                    g_outgoing.erase(it);
                    found = true;
                }
            }

            if (found)
            {
                NotifyTransferEvent(
                    transferID,
                    transaction.peer,
                    STRPM::InventoryTransferState::kFailed,
                    result,
                    transaction.entry);
            }
        }

        void RollbackOutgoingOnGameThread(
            STRPM::InventoryTransferID transferID,
            STRPM::Result failureResult)
        {
            OutgoingTransaction transaction{};
            bool found = false;
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(transferID);
                if (it != g_outgoing.end())
                {
                    transaction = it->second;
                    found = true;
                }
            }
            if (!found)
                return;

            Native::Entry nativeEntry{};
            try
            {
                if (Native::ToNative(
                        transaction.entry,
                        nativeEntry) != STRPM::Result::kOk)
                {
                    FailOutgoing(
                        transferID,
                        failureResult);
                    return;
                }
            }
            catch (...)
            {
                FailOutgoing(
                    transferID,
                    failureResult);
                return;
            }

            nativeEntry.Count =
                std::abs(transaction.entry.count);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player)
                InvokeAddOrRemoveItem(player, &nativeEntry);

            {
                std::scoped_lock lock(g_transactionMutex);
                g_outgoing.erase(transferID);
            }
            NotifyTransferEvent(
                transferID,
                transaction.peer,
                STRPM::InventoryTransferState::kRolledBack,
                failureResult,
                transaction.entry);
        }

        void RemoveLocalAndCommitOnGameThread(
            STRPM::InventoryTransferID transferID)
        {
            OutgoingTransaction transaction{};
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(transferID);
                if (it == g_outgoing.end() ||
                    it->second.phase != OutgoingPhase::kRemoving)
                {
                    return;
                }
                transaction = it->second;
            }

            const auto available =
                GetMatchingLocalCount(transaction.entry);
            if (available < transaction.entry.count)
            {
                SendWire(
                    transaction.peer,
                    WireType::kAbort,
                    transferID,
                    transaction.entry,
                    STRPM::Result::kTargetNotFound);
                FailOutgoing(
                    transferID,
                    STRPM::Result::kTargetNotFound);
                return;
            }

            Native::Entry nativeEntry{};
            try
            {
                if (Native::ToNative(
                        transaction.entry,
                        nativeEntry) != STRPM::Result::kOk)
                {
                    SendWire(
                        transaction.peer,
                        WireType::kAbort,
                        transferID,
                        transaction.entry,
                        STRPM::Result::kUnsupportedItem);
                    FailOutgoing(
                        transferID,
                        STRPM::Result::kUnsupportedItem);
                    return;
                }
            }
            catch (...)
            {
                FailOutgoing(
                    transferID,
                    STRPM::Result::kTransportError);
                return;
            }

            nativeEntry.Count =
                -std::abs(transaction.entry.count);

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player ||
                !InvokeAddOrRemoveItem(player, &nativeEntry))
            {
                SendWire(
                    transaction.peer,
                    WireType::kAbort,
                    transferID,
                    transaction.entry,
                    STRPM::Result::kTransportError);
                FailOutgoing(
                    transferID,
                    STRPM::Result::kTransportError);
                return;
            }

            const auto after =
                GetMatchingLocalCount(transaction.entry);
            if (after > available - transaction.entry.count)
            {
                // The native call did not remove the requested exact instance.
                // Re-add only what was observed as removed, if any.
                const auto removed = std::max(
                    0,
                    available - after);
                if (removed > 0)
                {
                    nativeEntry.Count = removed;
                    InvokeAddOrRemoveItem(player, &nativeEntry);
                }

                SendWire(
                    transaction.peer,
                    WireType::kAbort,
                    transferID,
                    transaction.entry,
                    STRPM::Result::kTransportError);
                FailOutgoing(
                    transferID,
                    STRPM::Result::kTransportError);
                return;
            }

            NotifyTransferEvent(
                transferID,
                transaction.peer,
                STRPM::InventoryTransferState::kCommitting,
                STRPM::Result::kOk,
                transaction.entry);

            const auto sent = SendWire(
                transaction.peer,
                WireType::kCommit,
                transferID,
                transaction.entry);
            if (sent != STRPM::Result::kOk)
            {
                // COMMIT was not accepted by STRPM transport, so the recipient
                // cannot have observed it. A local rollback is safe here.
                RollbackOutgoingOnGameThread(
                    transferID,
                    sent);
                return;
            }

            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(transferID);
                if (it != g_outgoing.end())
                {
                    it->second.phase =
                        OutgoingPhase::kAwaitApplied;
                    it->second.commitAttempts = 1;
                    it->second.lastAction =
                        std::chrono::steady_clock::now();
                }
            }
        }

        void ApplyRemoteOnGameThread(
            TransactionKey key)
        {
            IncomingTransaction transaction{};
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_incoming.find(key);
                if (it == g_incoming.end())
                    return;
                transaction = it->second;
            }

            Native::Entry nativeEntry{};
            try
            {
                if (Native::ToNative(
                        transaction.entry,
                        nativeEntry) != STRPM::Result::kOk)
                {
                    SendWire(
                        key.sender,
                        WireType::kAbort,
                        key.transferID,
                        transaction.entry,
                        STRPM::Result::kUnsupportedItem);
                    return;
                }
            }
            catch (...)
            {
                SendWire(
                    key.sender,
                    WireType::kAbort,
                    key.transferID,
                    transaction.entry,
                    STRPM::Result::kTransportError);
                return;
            }

            nativeEntry.Count =
                std::abs(transaction.entry.count);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player ||
                !InvokeAddOrRemoveItem(player, &nativeEntry))
            {
                SendWire(
                    key.sender,
                    WireType::kAbort,
                    key.transferID,
                    transaction.entry,
                    STRPM::Result::kTransportError);
                return;
            }

            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_incoming.find(key);
                if (it != g_incoming.end())
                {
                    it->second.applied = true;
                    it->second.applyScheduled = false;
                    it->second.lastAction =
                        std::chrono::steady_clock::now();
                }
            }

            SendWire(
                key.sender,
                WireType::kApplied,
                key.transferID,
                transaction.entry);
            NotifyTransferEvent(
                key.transferID,
                key.sender,
                STRPM::InventoryTransferState::kComplete,
                STRPM::Result::kOk,
                transaction.entry);
        }

        void ValidateIncomingOnGameThread(
            TransactionKey key)
        {
            IncomingTransaction transaction{};
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_incoming.find(key);
                if (it == g_incoming.end() ||
                    !it->second.validationScheduled)
                {
                    return;
                }
                transaction = it->second;
            }

            if (!ValidatePortableEntryOnGameThread(
                    transaction.entry))
            {
                {
                    std::scoped_lock lock(g_transactionMutex);
                    g_incoming.erase(key);
                }
                SendWire(
                    key.sender,
                    WireType::kAbort,
                    key.transferID,
                    transaction.entry,
                    STRPM::Result::kUnsupportedItem);
                return;
            }

            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_incoming.find(key);
                if (it == g_incoming.end())
                    return;
                it->second.validationScheduled = false;
                it->second.ready = true;
                it->second.lastAction =
                    std::chrono::steady_clock::now();
            }

            SendWire(
                key.sender,
                WireType::kReady,
                key.transferID,
                transaction.entry);
            NotifyTransferEvent(
                key.transferID,
                key.sender,
                STRPM::InventoryTransferState::kPrepared,
                STRPM::Result::kOk,
                transaction.entry);
        }

        void HandlePrepare(
            const STRPM::Message* message,
            const WireMessage& wire)
        {
            if (!ValidateEntryStructure(wire.entry))
            {
                SendWire(
                    message->sender.connectionID,
                    WireType::kAbort,
                    wire.transferID,
                    wire.entry,
                    STRPM::Result::kUnsupportedItem);
                return;
            }

            const TransactionKey key{
                message->sender.connectionID,
                wire.transferID
            };

            bool alreadyApplied = false;
            bool alreadyReady = false;
            bool mismatch = false;
            bool scheduleValidation = false;
            {
                std::scoped_lock lock(g_transactionMutex);
                auto [it, inserted] =
                    g_incoming.try_emplace(key);
                if (inserted)
                {
                    it->second.entry = wire.entry;
                    it->second.validationScheduled = true;
                    it->second.lastAction =
                        std::chrono::steady_clock::now();
                    scheduleValidation = true;
                }
                else
                {
                    mismatch =
                        !EntriesEquivalentIgnoringCount(
                            it->second.entry,
                            wire.entry) ||
                        it->second.entry.count !=
                            wire.entry.count;
                    alreadyApplied = it->second.applied;
                    alreadyReady = it->second.ready;
                    if (!mismatch &&
                        !alreadyApplied &&
                        !alreadyReady &&
                        !it->second.validationScheduled)
                    {
                        it->second.validationScheduled = true;
                        scheduleValidation = true;
                    }
                    it->second.lastAction =
                        std::chrono::steady_clock::now();
                }
            }

            if (mismatch)
            {
                SendWire(
                    message->sender.connectionID,
                    WireType::kAbort,
                    wire.transferID,
                    wire.entry,
                    STRPM::Result::kInvalidArgument);
                return;
            }

            if (alreadyApplied)
            {
                SendWire(
                    message->sender.connectionID,
                    WireType::kApplied,
                    wire.transferID,
                    wire.entry);
                return;
            }

            if (alreadyReady)
            {
                SendWire(
                    message->sender.connectionID,
                    WireType::kReady,
                    wire.transferID,
                    wire.entry);
                return;
            }

            if (!scheduleValidation)
                return;

            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface)
            {
                {
                    std::scoped_lock lock(g_transactionMutex);
                    g_incoming.erase(key);
                }
                SendWire(
                    message->sender.connectionID,
                    WireType::kAbort,
                    wire.transferID,
                    wire.entry,
                    STRPM::Result::kNotAvailable);
                return;
            }

            taskInterface->AddTask(
                [key]() {
                    ValidateIncomingOnGameThread(key);
                });
        }

        void HandleReady(
            const STRPM::Message* message,
            const WireMessage& wire)
        {
            bool schedule = false;
            STRPM::InventoryEntry entry{};
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(wire.transferID);
                if (it == g_outgoing.end() ||
                    it->second.peer !=
                        message->sender.connectionID ||
                    it->second.phase !=
                        OutgoingPhase::kAwaitReady)
                {
                    return;
                }

                it->second.phase = OutgoingPhase::kRemoving;
                it->second.lastAction =
                    std::chrono::steady_clock::now();
                entry = it->second.entry;
                schedule = true;
            }

            if (!schedule)
                return;

            NotifyTransferEvent(
                wire.transferID,
                message->sender.connectionID,
                STRPM::InventoryTransferState::kRemovingLocal,
                STRPM::Result::kOk,
                entry);

            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface)
            {
                FailOutgoing(
                    wire.transferID,
                    STRPM::Result::kNotAvailable);
                return;
            }

            taskInterface->AddTask(
                [transferID = wire.transferID]() {
                    RemoveLocalAndCommitOnGameThread(
                        transferID);
                });
        }

        void HandleCommit(
            const STRPM::Message* message,
            const WireMessage& wire)
        {
            const TransactionKey key{
                message->sender.connectionID,
                wire.transferID
            };

            bool sendApplied = false;
            bool schedule = false;
            STRPM::InventoryEntry entry{};
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_incoming.find(key);
                if (it == g_incoming.end() ||
                    !it->second.ready)
                {
                    // A COMMIT without a validated PREPARE is not accepted.
                    return;
                }

                it->second.lastAction =
                    std::chrono::steady_clock::now();
                entry = it->second.entry;
                if (it->second.applied)
                {
                    sendApplied = true;
                }
                else if (!it->second.applyScheduled)
                {
                    it->second.applyScheduled = true;
                    schedule = true;
                }
            }

            if (sendApplied)
            {
                SendWire(
                    message->sender.connectionID,
                    WireType::kApplied,
                    wire.transferID,
                    entry);
                return;
            }

            if (!schedule)
                return;

            NotifyTransferEvent(
                wire.transferID,
                message->sender.connectionID,
                STRPM::InventoryTransferState::kApplyingRemote,
                STRPM::Result::kOk,
                entry);

            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface)
            {
                SendWire(
                    message->sender.connectionID,
                    WireType::kAbort,
                    wire.transferID,
                    entry,
                    STRPM::Result::kNotAvailable);
                return;
            }

            taskInterface->AddTask(
                [key]() {
                    ApplyRemoteOnGameThread(key);
                });
        }

        void HandleApplied(
            const STRPM::Message* message,
            const WireMessage& wire)
        {
            OutgoingTransaction transaction{};
            bool found = false;
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(wire.transferID);
                if (it != g_outgoing.end() &&
                    it->second.peer ==
                        message->sender.connectionID &&
                    it->second.phase ==
                        OutgoingPhase::kAwaitApplied)
                {
                    transaction = it->second;
                    g_outgoing.erase(it);
                    found = true;
                }
            }

            if (found)
            {
                NotifyTransferEvent(
                    wire.transferID,
                    transaction.peer,
                    STRPM::InventoryTransferState::kComplete,
                    STRPM::Result::kOk,
                    transaction.entry);
            }
        }

        void HandleAbort(
            const STRPM::Message* message,
            const WireMessage& wire)
        {
            const auto result =
                static_cast<STRPM::Result>(wire.result);

            OutgoingTransaction transaction{};
            bool found = false;
            bool removedMayHaveOccurred = false;
            {
                std::scoped_lock lock(g_transactionMutex);
                const auto it = g_outgoing.find(wire.transferID);
                if (it != g_outgoing.end() &&
                    it->second.peer ==
                        message->sender.connectionID)
                {
                    transaction = it->second;
                    removedMayHaveOccurred =
                        it->second.phase !=
                            OutgoingPhase::kAwaitReady;
                    found = true;
                }
            }

            if (!found)
                return;

            if (removedMayHaveOccurred)
            {
                // Once COMMIT may have reached the recipient, automatically
                // re-adding locally can duplicate the item. We therefore report
                // a failed/uncertain transfer rather than performing an unsafe
                // rollback. Immediate local send failures are handled separately
                // before COMMIT can leave this process.
                FailOutgoing(wire.transferID, result);
            }
            else
            {
                FailOutgoing(wire.transferID, result);
            }
        }

        void STRPM_CALL ReceiveInventoryMessage(
            const STRPM::Message* message,
            void*)
        {
            if (!message ||
                message->sender.connectionID == 0 ||
                !message->data ||
                message->size != sizeof(WireMessage))
            {
                return;
            }

            WireMessage wire{};
            std::memcpy(
                &wire,
                message->data,
                sizeof(wire));
            if (wire.magic != kWireMagic ||
                wire.version != kWireVersion ||
                wire.transferID == 0)
            {
                return;
            }

            switch (wire.type)
            {
            case WireType::kPrepare:
                HandlePrepare(message, wire);
                break;
            case WireType::kReady:
                HandleReady(message, wire);
                break;
            case WireType::kCommit:
                HandleCommit(message, wire);
                break;
            case WireType::kApplied:
                HandleApplied(message, wire);
                break;
            case WireType::kAbort:
                HandleAbort(message, wire);
                break;
            default:
                break;
            }
        }

        bool EnsureChannelRegistered() noexcept
        {
            if (g_channelRegistered.load())
                return true;

            const auto* api = QueryMessagingApi();
            if (!api || !api->registerChannel)
                return false;

            STRPM::ListenerHandle handle{};
            const auto result = api->registerChannel(
                kInventoryChannel,
                &ReceiveInventoryMessage,
                nullptr,
                &handle);
            if (result != STRPM::Result::kOk)
                return false;

            g_inventoryChannelHandle = handle;
            g_channelRegistered.store(true);
            Log(
                "STRPM inventory channel registered: %s",
                kInventoryChannel);
            return true;
        }

        void ProcessTimeouts()
        {
            struct Resend
            {
                STRPM::InventoryTransferID transferID;
                STRPM::ConnectionID peer;
                STRPM::InventoryEntry entry;
            };

            std::vector<STRPM::InventoryTransferID> failures;
            std::vector<Resend> resends;
            const auto now = std::chrono::steady_clock::now();

            {
                std::scoped_lock lock(g_transactionMutex);
                for (auto& [id, transaction] : g_outgoing)
                {
                    if (transaction.phase ==
                            OutgoingPhase::kAwaitReady &&
                        now - transaction.lastAction >
                            kPrepareTimeout)
                    {
                        failures.push_back(id);
                    }
                    else if (transaction.phase ==
                                 OutgoingPhase::kAwaitApplied &&
                             now - transaction.lastAction >
                                 kCommitRetryInterval)
                    {
                        if (transaction.commitAttempts <
                            kMaxCommitAttempts)
                        {
                            ++transaction.commitAttempts;
                            transaction.lastAction = now;
                            resends.push_back({
                                id,
                                transaction.peer,
                                transaction.entry
                            });
                        }
                        else
                        {
                            // Do not rollback here: the remote side may already
                            // have applied COMMIT and only the ACK was lost.
                            failures.push_back(id);
                        }
                    }
                }

                for (auto it = g_incoming.begin();
                     it != g_incoming.end();)
                {
                    if (it->second.applied &&
                        now - it->second.lastAction >
                            kIncomingRetention)
                    {
                        it = g_incoming.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            for (const auto& resend : resends)
            {
                SendWire(
                    resend.peer,
                    WireType::kCommit,
                    resend.transferID,
                    resend.entry);
            }

            for (const auto id : failures)
                FailOutgoing(id, STRPM::Result::kTransportError);
        }

        void WorkerLoop(std::stop_token token)
        {
            bool nativeLoggedWaiting = false;
            auto nextResolveAttempt =
                std::chrono::steady_clock::time_point{};
            while (!token.stop_requested())
            {
                EnsureChannelRegistered();

                const auto now = std::chrono::steady_clock::now();
                const bool nativeResolved =
                    g_getItemFromExtraDataAddress.load() != 0 &&
                    g_addOrRemoveItemAddress.load() != 0;
                if (!nativeResolved &&
                    (nextResolveAttempt.time_since_epoch().count() == 0 ||
                     now >= nextResolveAttempt))
                {
                    nextResolveAttempt = now + 2s;
                    if (!ResolveNativeFunctions())
                    {
                        if (!nativeLoggedWaiting)
                        {
                            Log(
                                "STRPM inventory native bridge waiting for STR 1.8.4 runtime");
                            nativeLoggedWaiting = true;
                        }
                    }
                    else
                    {
                        nativeLoggedWaiting = false;
                    }
                }

                const bool ready =
                    g_channelRegistered.load() &&
                    g_getItemFromExtraDataAddress.load() != 0 &&
                    g_addOrRemoveItemAddress.load() != 0;
                g_ready.store(ready);

                ProcessTimeouts();

                for (int i = 0;
                     i < 5 && !token.stop_requested();
                     ++i)
                {
                    std::this_thread::sleep_for(50ms);
                }
            }
        }

        STRPM::Result STRPM_CALL GetLocalInventory(
            STRPM::InventoryItem* items,
            std::size_t capacity,
            std::size_t* inOutCount)
        {
            if (!inOutCount)
                return STRPM::Result::kInvalidArgument;
            if (!g_ready.load())
                return STRPM::Result::kNotAvailable;

            std::vector<STRPM::InventoryItem> snapshot;
            const auto result = BuildLocalInventory(snapshot);
            if (result != STRPM::Result::kOk)
                return result;

            const auto required = snapshot.size();
            *inOutCount = required;

            if (!items || capacity == 0)
                return STRPM::Result::kOk;
            if (capacity < required)
                return STRPM::Result::kBufferTooSmall;

            std::copy(
                snapshot.begin(),
                snapshot.end(),
                items);
            return STRPM::Result::kOk;
        }

        STRPM::Result STRPM_CALL Transfer(
            STRPM::ConnectionID recipientConnectionID,
            const STRPM::InventoryEntry* entry,
            STRPM::InventoryTransferID* outTransferID)
        {
            if (!entry ||
                !outTransferID ||
                recipientConnectionID == 0)
            {
                return STRPM::Result::kInvalidArgument;
            }

            *outTransferID = 0;
            if (!g_ready.load())
                return STRPM::Result::kNotAvailable;
            if (!STRPMProxyResolverBridge::IsSTRSessionConnected())
                return STRPM::Result::kNotConnected;

            if (!ValidateEntryStructure(*entry))
                return STRPM::Result::kUnsupportedItem;

            const auto transferID =
                g_nextTransferID.fetch_add(1);
            OutgoingTransaction transaction{};
            transaction.peer = recipientConnectionID;
            transaction.entry = *entry;
            transaction.phase = OutgoingPhase::kAwaitReady;
            transaction.lastAction =
                std::chrono::steady_clock::now();

            {
                std::scoped_lock lock(g_transactionMutex);
                g_outgoing.emplace(
                    transferID,
                    transaction);
            }

            const auto sent = SendWire(
                recipientConnectionID,
                WireType::kPrepare,
                transferID,
                *entry);
            if (sent != STRPM::Result::kOk)
            {
                std::scoped_lock lock(g_transactionMutex);
                g_outgoing.erase(transferID);
                return sent;
            }

            *outTransferID = transferID;
            NotifyTransferEvent(
                transferID,
                recipientConnectionID,
                STRPM::InventoryTransferState::kPrepared,
                STRPM::Result::kOk,
                *entry);
            return STRPM::Result::kOk;
        }

        STRPM::Result STRPM_CALL RegisterTransferListener(
            STRPM::InventoryTransferCallback callback,
            void* userData)
        {
            if (!callback)
                return STRPM::Result::kInvalidArgument;

            std::scoped_lock lock(g_listenerMutex);
            const auto it = std::find_if(
                g_listeners.begin(),
                g_listeners.end(),
                [&](const auto& listener) {
                    return listener.callback == callback &&
                           listener.userData == userData;
                });
            if (it == g_listeners.end())
                g_listeners.push_back({ callback, userData });
            return STRPM::Result::kOk;
        }

        STRPM::Result STRPM_CALL UnregisterTransferListener(
            STRPM::InventoryTransferCallback callback,
            void* userData)
        {
            if (!callback)
                return STRPM::Result::kInvalidArgument;

            std::scoped_lock lock(g_listenerMutex);
            const auto oldSize = g_listeners.size();
            std::erase_if(
                g_listeners,
                [&](const auto& listener) {
                    return listener.callback == callback &&
                           listener.userData == userData;
                });
            return oldSize == g_listeners.size() ?
                STRPM::Result::kNotAvailable :
                STRPM::Result::kOk;
        }

        const STRPM::InventoryInterface g_interface{
            STRPM::kInventoryInterfaceVersion,
            &GetLocalInventory,
            &Transfer,
            &RegisterTransferListener,
            &UnregisterTransferListener
        };
    }

    void Start() noexcept
    {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true))
            return;

        try
        {
            g_worker = std::jthread(&WorkerLoop);
            Log(
                "STRPM inventory bridge starting for %s",
                STRPMSTRCompatibility::kTargetBuild);
        }
        catch (...)
        {
            g_started.store(false);
            g_ready.store(false);
            Log(
                "STRPM inventory bridge failed to start worker");
        }
    }

    void Stop() noexcept
    {
        if (!g_started.exchange(false))
            return;

        if (g_worker.joinable())
        {
            g_worker.request_stop();
            g_worker.join();
        }

        if (g_channelRegistered.exchange(false))
        {
            const auto* api = QueryMessagingApi();
            if (api && api->unregisterChannel)
            {
                api->unregisterChannel(
                    g_inventoryChannelHandle);
            }
        }

        {
            std::scoped_lock lock(g_transactionMutex);
            g_outgoing.clear();
            g_incoming.clear();
        }
        {
            std::scoped_lock lock(g_listenerMutex);
            g_listeners.clear();
        }

        g_ready.store(false);
        g_getItemFromExtraDataAddress.store(0);
        g_addOrRemoveItemAddress.store(0);
    }

    bool IsReady() noexcept
    {
        return g_ready.load();
    }

    STRPM::Result QueryInterface(
        std::uint32_t requestedVersion,
        const STRPM::InventoryInterface** outInterface) noexcept
    {
        if (!outInterface)
            return STRPM::Result::kInvalidArgument;

        *outInterface = nullptr;
        if (requestedVersion !=
            STRPM::kInventoryInterfaceVersion)
        {
            return STRPM::Result::kUnsupportedVersion;
        }

        if (!IsReady())
            return STRPM::Result::kNotAvailable;

        *outInterface = &g_interface;
        return STRPM::Result::kOk;
    }
}

STRPM_EXPORT STRPM::Result STRPM_CALL STR_QueryPluginMessagingInventory(
    std::uint32_t requestedVersion,
    const STRPM::InventoryInterface** outInterface)
{
    return STRPMInventoryBridge::QueryInterface(
        requestedVersion,
        outInterface);
}
