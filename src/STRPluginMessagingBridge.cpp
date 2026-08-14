#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr char kTargetBuild[] = "Skyrim Together Reborn 1.8.0";
    constexpr char kSendChatAnchor[] = "Send chat message of type {}: '{}' ";
    constexpr std::size_t kMaxEnvelopeChars = 3500;
    constexpr std::size_t kMaxFragments = 64;
    constexpr std::size_t kMaxCallDistanceAfterAnchor = 0x200;

    struct MemorySpan
    {
        std::uintptr_t base{ 0 };
        std::size_t size{ 0 };
        bool readable{ false };
        bool executable{ false };
        HMODULE allocationModule{ nullptr };
    };

    struct FunctionBounds
    {
        std::uintptr_t begin{ 0 };
        std::uintptr_t end{ 0 };
    };

    struct CallSite
    {
        std::uintptr_t site{ 0 };
        std::uintptr_t target{ 0 };
        std::size_t distanceAfterAnchor{ 0 };
    };

    std::mutex g_lock;
    STRPM::ReceiveCallback g_callback = nullptr;
    void* g_userData = nullptr;
    std::string g_displayName;
    std::atomic<std::uint64_t> g_sequence{ 1 };

    HMODULE g_selfModule = nullptr;
    std::vector<MemorySpan> g_memory;
    std::vector<std::uintptr_t> g_anchorAddresses;
    std::vector<std::uintptr_t> g_xrefs;
    std::vector<CallSite> g_processChatCalls;
    std::optional<FunctionBounds> g_processChatBounds;

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

    void ResolveSelfModule()
    {
        if (g_selfModule)
            return;

        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&ResolveSelfModule),
                &module))
        {
            g_selfModule = module;
        }
    }

    bool IsReadableProtection(DWORD protect)
    {
        if ((protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS)
            return false;
        const DWORD p = protect & 0xFF;
        return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
               p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    }

    bool IsExecutableProtection(DWORD protect)
    {
        if ((protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS)
            return false;
        const DWORD p = protect & 0xFF;
        return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
               p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    }

    HMODULE ModuleForAllocationBase(void* allocationBase)
    {
        if (!allocationBase)
            return nullptr;

        char path[MAX_PATH]{};
        const auto module = reinterpret_cast<HMODULE>(allocationBase);
        if (GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))) == 0)
            return nullptr;
        return module;
    }

    std::string ModuleNameForAddress(std::uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
            return "<unknown>";

        const auto module = ModuleForAllocationBase(mbi.AllocationBase);
        if (!module)
            return "<private-mapped>";

        char path[MAX_PATH]{};
        if (GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))) == 0)
            return "<module>";

        const char* slash = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
        return slash ? slash + 1 : path;
    }

    void EnumerateMemory()
    {
        ResolveSelfModule();
        g_memory.clear();

        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        auto current = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

        while (current < maximum)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi)) != sizeof(mbi))
                break;

            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto size = static_cast<std::size_t>(mbi.RegionSize);
            const auto allocationModule = ModuleForAllocationBase(mbi.AllocationBase);

            if (mbi.State == MEM_COMMIT && size != 0 && allocationModule != g_selfModule)
            {
                const bool readable = IsReadableProtection(mbi.Protect);
                const bool executable = IsExecutableProtection(mbi.Protect);
                if (readable || executable)
                    g_memory.push_back(MemorySpan{ base, size, readable, executable, allocationModule });
            }

            if (size == 0 || base + size <= current)
                break;
            current = base + size;
        }
    }

    void FindAnchorCopies()
    {
        g_anchorAddresses.clear();
        const auto* needle = reinterpret_cast<const std::uint8_t*>(kSendChatAnchor);
        constexpr std::size_t needleSize = sizeof(kSendChatAnchor) - 1;

        for (const auto& span : g_memory)
        {
            if (!span.readable || span.size < needleSize)
                continue;

            const auto* bytes = reinterpret_cast<const std::uint8_t*>(span.base);
            for (std::size_t i = 0; i + needleSize <= span.size; ++i)
            {
                if (std::memcmp(bytes + i, needle, needleSize) == 0)
                    g_anchorAddresses.push_back(span.base + i);
            }
        }
    }

    void FindRipRelativeXrefs()
    {
        g_xrefs.clear();
        if (g_anchorAddresses.empty())
            return;

        for (const auto& span : g_memory)
        {
            if (!span.executable || span.size < 7)
                continue;

            const auto* code = reinterpret_cast<const std::uint8_t*>(span.base);
            for (std::size_t i = 0; i + 7 <= span.size; ++i)
            {
                if ((code[i] & 0xF8) != 0x48 || code[i + 1] != 0x8D)
                    continue;

                const std::uint8_t modrm = code[i + 2];
                if ((modrm & 0xC7) != 0x05)
                    continue;

                std::int32_t displacement = 0;
                std::memcpy(&displacement, code + i + 3, sizeof(displacement));
                const auto instruction = span.base + i;
                const auto target = instruction + 7 + static_cast<std::intptr_t>(displacement);
                if (std::find(g_anchorAddresses.begin(), g_anchorAddresses.end(), target) != g_anchorAddresses.end())
                    g_xrefs.push_back(instruction);
            }
        }
    }

    std::optional<FunctionBounds> GetFunctionBounds(std::uintptr_t address)
    {
        DWORD64 imageBase = 0;
        const auto runtimeFunction = RtlLookupFunctionEntry(
            static_cast<DWORD64>(address),
            &imageBase,
            nullptr);
        if (!runtimeFunction)
            return std::nullopt;

        const auto begin = static_cast<std::uintptr_t>(imageBase + runtimeFunction->BeginAddress);
        const auto end = static_cast<std::uintptr_t>(imageBase + runtimeFunction->EndAddress);
        if (begin == 0 || end <= begin || address < begin || address >= end)
            return std::nullopt;
        return FunctionBounds{ begin, end };
    }

    bool IsCommittedExecutable(std::uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        return mbi.State == MEM_COMMIT && IsExecutableProtection(mbi.Protect);
    }

    std::vector<CallSite> EnumerateDirectCalls(const FunctionBounds& bounds, std::uintptr_t anchorXref)
    {
        std::vector<CallSite> result;
        if (bounds.end <= bounds.begin)
            return result;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(bounds.begin), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect))
        {
            return result;
        }

        const auto* code = reinterpret_cast<const std::uint8_t*>(bounds.begin);
        const auto size = bounds.end - bounds.begin;
        for (std::size_t i = 0; i + 5 <= size; ++i)
        {
            if (code[i] != 0xE8)
                continue;

            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + i + 1, sizeof(displacement));
            const auto site = bounds.begin + i;
            const auto target = site + 5 + static_cast<std::intptr_t>(displacement);
            if (!IsCommittedExecutable(target))
                continue;

            const auto distance = site > anchorXref ? static_cast<std::size_t>(site - anchorXref) : 0;
            result.push_back(CallSite{ site, target, distance });
        }
        return result;
    }

    bool ResolveProcessChatMessage()
    {
        g_processChatBounds.reset();
        g_processChatCalls.clear();
        g_anchorAddresses.clear();
        g_xrefs.clear();

        EnumerateMemory();
        FindAnchorCopies();
        FindRipRelativeXrefs();

        // The public 1.8.0 executable is packed. At runtime the actual client code
        // may live in a private mapped image rather than the raw PE sections, so we
        // deliberately resolve from process memory instead of .text/.rdata.
        std::vector<std::uintptr_t> viableXrefs;
        for (const auto xref : g_xrefs)
        {
            const auto bounds = GetFunctionBounds(xref);
            if (bounds)
                viableXrefs.push_back(xref);
        }

        if (viableXrefs.size() != 1)
        {
            Log("resolver rejected build: expected exactly one unwind-backed chat xref, found %zu", viableXrefs.size());
            return false;
        }

        const auto xref = viableXrefs.front();
        g_xrefs = { xref };
        g_processChatBounds = GetFunctionBounds(xref);
        if (!g_processChatBounds)
            return false;

        g_processChatCalls = EnumerateDirectCalls(*g_processChatBounds, xref);
        std::ranges::sort(g_processChatCalls, [](const CallSite& lhs, const CallSite& rhs) {
            return lhs.site < rhs.site;
        });
        return !g_processChatCalls.empty();
    }

    char HexDigit(unsigned value)
    {
        static constexpr char digits[] = "0123456789ABCDEF";
        return digits[value & 0xF];
    }

    std::string HexEncode(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::string result;
        result.resize(size * 2);
        for (std::size_t i = 0; i < size; ++i)
        {
            result[i * 2] = HexDigit(bytes[i] >> 4);
            result[i * 2 + 1] = HexDigit(bytes[i]);
        }
        return result;
    }

    bool IsChannelSafe(const char* channel)
    {
        if (!channel || !channel[0])
            return false;
        const auto length = std::strlen(channel);
        if (length > STRPM::kMaxChannelLength)
            return false;
        return std::strchr(channel, '|') == nullptr && std::strchr(channel, '=') == nullptr;
    }

    std::optional<std::string> TargetField(const STRPM::Target& target)
    {
        switch (target.kind)
        {
        case STRPM::TargetKind::kServer:
            return std::string("server");
        case STRPM::TargetKind::kPlayer:
            if (target.connectionID == 0)
                return std::nullopt;
            return std::string("id:") + std::to_string(target.connectionID);
        case STRPM::TargetKind::kAllPlayers:
            return std::string("all");
        case STRPM::TargetKind::kHost:
            // The documented STR server scripting API does not expose a stable
            // host lookup. Do not silently turn host into broadcast.
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    std::vector<std::string> BuildEnvelopes(
        const char* channel,
        const STRPM::Target& target,
        const void* data,
        std::size_t size,
        std::uint32_t flags,
        std::uint64_t sequence)
    {
        std::vector<std::string> result;
        const auto targetField = TargetField(target);
        if (!targetField)
            return result;

        const auto payloadHex = HexEncode(data, size);
        const auto messageId = std::to_string(GetCurrentProcessId()) + "-" +
                               std::to_string(GetTickCount64()) + "-" +
                               std::to_string(sequence);

        std::ostringstream prefix;
        prefix << "STRPM|v2|msg=" << messageId
               << "|seq=" << sequence
               << "|channel=" << channel
               << "|target=" << *targetField
               << "|flags=" << flags;

        // Reserve enough room for part/parts/payload fields and decimal fragment
        // counters. Keeping envelopes well below the server's 4096-character cap
        // gives room for authenticated sender metadata appended by the Lua relay.
        const std::string fixedPrefix = prefix.str();
        constexpr std::size_t overheadReserve = 96;
        if (fixedPrefix.size() + overheadReserve >= kMaxEnvelopeChars)
            return result;

        const auto payloadCharsPerFragment = kMaxEnvelopeChars - fixedPrefix.size() - overheadReserve;
        const auto fragmentCount = std::max<std::size_t>(1, (payloadHex.size() + payloadCharsPerFragment - 1) / payloadCharsPerFragment);
        if (fragmentCount > kMaxFragments)
            return result;

        result.reserve(fragmentCount);
        for (std::size_t index = 0; index < fragmentCount; ++index)
        {
            const auto offset = index * payloadCharsPerFragment;
            const auto count = std::min(payloadCharsPerFragment, payloadHex.size() - std::min(offset, payloadHex.size()));

            std::ostringstream envelope;
            envelope << fixedPrefix
                     << "|part=" << (index + 1)
                     << "|parts=" << fragmentCount
                     << "|payload=";
            if (count != 0)
                envelope << payloadHex.substr(offset, count);
            result.push_back(envelope.str());
        }
        return result;
    }

    void ProbeSTR180()
    {
        FILE* truncate = nullptr;
        fopen_s(&truncate, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "w");
        if (truncate)
            fclose(truncate);

        Log("STRPM bridge resolver target: %s", kTargetBuild);
        Log("source tag: tiltedphoques/TiltedEvolution v1.8.0 (9c23efa422bbc1e5c06eef5522ca73971a513e35)");
        Log("Nexus public exe SHA-256: 77f23c9c82c412252b5c4491a09d7ab4349cbc6c77992c4766882f54798cb99d");

        const bool resolved = ResolveProcessChatMessage();
        Log("mapped memory spans considered: %zu", g_memory.size());
        Log("send-chat anchor copies outside bridge module: %zu", g_anchorAddresses.size());
        for (const auto address : g_anchorAddresses)
            Log("  anchor = 0x%llX module=%s",
                static_cast<unsigned long long>(address),
                ModuleNameForAddress(address).c_str());

        Log("unwind-backed send-chat RIP xrefs: %zu", g_xrefs.size());
        for (const auto address : g_xrefs)
            Log("  xref = 0x%llX module=%s",
                static_cast<unsigned long long>(address),
                ModuleNameForAddress(address).c_str());

        if (g_processChatBounds && !g_xrefs.empty())
        {
            Log("ProcessChatMessage candidate: 0x%llX-0x%llX module=%s",
                static_cast<unsigned long long>(g_processChatBounds->begin),
                static_cast<unsigned long long>(g_processChatBounds->end),
                ModuleNameForAddress(g_processChatBounds->begin).c_str());
            Log("direct CALL candidates in ProcessChatMessage: %zu", g_processChatCalls.size());
            for (std::size_t i = 0; i < g_processChatCalls.size(); ++i)
            {
                const auto& call = g_processChatCalls[i];
                const bool nearAfterAnchor = call.site > g_xrefs.front() &&
                                             call.distanceAfterAnchor <= kMaxCallDistanceAfterAnchor;
                Log("  call[%zu] site=0x%llX target=0x%llX distanceAfterAnchor=0x%zX targetModule=%s%s",
                    i,
                    static_cast<unsigned long long>(call.site),
                    static_cast<unsigned long long>(call.target),
                    call.distanceAfterAnchor,
                    ModuleNameForAddress(call.target).c_str(),
                    nearAfterAnchor ? " [near-after-anchor]" : "");
            }
        }

        Log("resolver status: %s", resolved
            ? "ProcessChatMessage located in mapped runtime; native send dispatch remains fail-safe until target ABI is validated"
            : "not resolved");
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
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL Stop()
    {
        std::scoped_lock lock(g_lock);
        g_callback = nullptr;
        g_userData = nullptr;
        return STRPM::Result::kOk;
    }

    STRPM::Result STRPM_CALL Send(
        const char* channel,
        STRPM::Target target,
        const void* data,
        std::size_t size,
        std::uint32_t flags)
    {
        if (!IsChannelSafe(channel) || (!data && size != 0))
            return STRPM::Result::kInvalidArgument;
        if (size > STRPM::kMaxPayloadBytes)
            return STRPM::Result::kPayloadTooLarge;
        if (target.kind == STRPM::TargetKind::kHost)
            return STRPM::Result::kTargetNotFound;

        const auto sequence = g_sequence.fetch_add(1);
        const auto envelopes = BuildEnvelopes(channel, target, data, size, flags, sequence);
        if (envelopes.empty())
            return STRPM::Result::kInvalidArgument;

        Log("prepared STRPM message seq=%llu channel=%s size=%zu fragments=%zu targetKind=%u",
            static_cast<unsigned long long>(sequence),
            channel,
            size,
            envelopes.size(),
            static_cast<unsigned>(target.kind));
        for (std::size_t i = 0; i < envelopes.size(); ++i)
            Log("  envelope[%zu] chars=%zu", i, envelopes[i].size());

        // Envelope generation is now complete. The final step is to feed each
        // envelope into STR's existing chat send path after the 1.8.0 runtime
        // call target and its owning TransportService instance are validated.
        // Until then, fail closed rather than call an uncertain address.
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL GetLocalConnectionID(STRPM::ConnectionID*)
    {
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL SetLocalDisplayName(const char* displayName)
    {
        std::scoped_lock lock(g_lock);
        g_displayName = displayName ? displayName : "";
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
