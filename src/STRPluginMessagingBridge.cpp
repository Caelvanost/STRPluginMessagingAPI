#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr char kTargetBuild[] = "Skyrim Together Reborn 1.8.0";
    constexpr char kTargetModule[] = "SkyrimTogether.exe";
    constexpr char kSendChatAnchor[] = "Send chat message of type {}: '{}' ";

    struct ModuleView
    {
        std::uintptr_t base{ 0 };
        std::size_t size{ 0 };
        std::uintptr_t textBase{ 0 };
        std::size_t textSize{ 0 };
        std::uintptr_t rdataBase{ 0 };
        std::size_t rdataSize{ 0 };
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
    };

    std::mutex g_lock;
    STRPM::ReceiveCallback g_callback = nullptr;
    void* g_userData = nullptr;
    std::vector<std::uintptr_t> g_anchorAddresses;
    std::vector<std::uintptr_t> g_xrefs;
    std::vector<CallSite> g_processChatCalls;
    std::optional<ModuleView> g_module;
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

    bool IsReadableAddress(std::uintptr_t address, std::size_t size)
    {
        if (address == 0 || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || mbi.Protect == PAGE_NOACCESS)
            return false;

        const auto regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBegin + static_cast<std::size_t>(mbi.RegionSize);
        return address >= regionBegin && address + size <= regionEnd;
    }

    std::optional<ModuleView> GetTargetModule()
    {
        const auto module = GetModuleHandleA(kTargetModule);
        if (!module)
            return std::nullopt;

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        if (!IsReadableAddress(base, sizeof(IMAGE_DOS_HEADER)))
            return std::nullopt;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return std::nullopt;

        const auto ntAddress = base + static_cast<std::uintptr_t>(dos->e_lfanew);
        if (!IsReadableAddress(ntAddress, sizeof(IMAGE_NT_HEADERS64)))
            return std::nullopt;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(ntAddress);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return std::nullopt;

        ModuleView result{};
        result.base = base;
        result.size = nt->OptionalHeader.SizeOfImage;

        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            char name[9]{};
            std::memcpy(name, section[i].Name, 8);
            const auto sectionBase = base + section[i].VirtualAddress;
            const auto sectionSize = static_cast<std::size_t>(std::max(section[i].Misc.VirtualSize, section[i].SizeOfRawData));

            if (std::strcmp(name, ".text") == 0)
            {
                result.textBase = sectionBase;
                result.textSize = sectionSize;
            }
            else if (std::strcmp(name, ".rdata") == 0)
            {
                result.rdataBase = sectionBase;
                result.rdataSize = sectionSize;
            }
        }

        if (result.textBase == 0 || result.textSize == 0 || result.rdataBase == 0 || result.rdataSize == 0)
            return std::nullopt;
        return result;
    }

    void FindAnchorCopies(const ModuleView& module)
    {
        g_anchorAddresses.clear();
        const auto* needle = reinterpret_cast<const std::uint8_t*>(kSendChatAnchor);
        constexpr std::size_t needleSize = sizeof(kSendChatAnchor) - 1;

        if (!IsReadableAddress(module.rdataBase, module.rdataSize) || module.rdataSize < needleSize)
            return;

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(module.rdataBase);
        for (std::size_t i = 0; i + needleSize <= module.rdataSize; ++i)
        {
            if (std::memcmp(bytes + i, needle, needleSize) == 0)
                g_anchorAddresses.push_back(module.rdataBase + i);
        }
    }

    void FindRipRelativeXrefs(const ModuleView& module)
    {
        g_xrefs.clear();
        if (g_anchorAddresses.empty() || !IsReadableAddress(module.textBase, module.textSize))
            return;

        const auto* code = reinterpret_cast<const std::uint8_t*>(module.textBase);
        for (std::size_t i = 0; i + 7 <= module.textSize; ++i)
        {
            // LEA r64, [RIP+disp32]. REX.W can be 48-4F depending on destination register.
            if ((code[i] & 0xF8) != 0x48 || code[i + 1] != 0x8D)
                continue;
            const std::uint8_t modrm = code[i + 2];
            if ((modrm & 0xC7) != 0x05)
                continue;

            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + i + 3, sizeof(displacement));
            const auto instruction = module.textBase + i;
            const auto target = instruction + 7 + static_cast<std::intptr_t>(displacement);
            if (std::ranges::find(g_anchorAddresses, target) != g_anchorAddresses.end())
                g_xrefs.push_back(instruction);
        }
    }

    std::optional<FunctionBounds> GetFunctionBounds(std::uintptr_t address, const ModuleView& module)
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
        if (begin < module.textBase || end > module.textBase + module.textSize || begin >= end)
            return std::nullopt;
        return FunctionBounds{ begin, end };
    }

    std::vector<CallSite> EnumerateDirectCalls(const FunctionBounds& bounds, const ModuleView& module)
    {
        std::vector<CallSite> result;
        if (bounds.end <= bounds.begin || !IsReadableAddress(bounds.begin, bounds.end - bounds.begin))
            return result;

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
            if (target >= module.textBase && target < module.textBase + module.textSize)
                result.push_back(CallSite{ site, target });
        }
        return result;
    }

    bool ResolveProcessChatMessage()
    {
        g_module = GetTargetModule();
        g_processChatBounds.reset();
        g_processChatCalls.clear();
        g_anchorAddresses.clear();
        g_xrefs.clear();

        if (!g_module)
        {
            Log("target module not loaded: %s", kTargetModule);
            return false;
        }

        FindAnchorCopies(*g_module);
        FindRipRelativeXrefs(*g_module);

        if (g_anchorAddresses.size() != 1 || g_xrefs.size() != 1)
        {
            Log("resolver rejected build: expected exactly one chat anchor and one xref");
            return false;
        }

        g_processChatBounds = GetFunctionBounds(g_xrefs.front(), *g_module);
        if (!g_processChatBounds)
        {
            Log("resolver could not obtain unwind bounds for ProcessChatMessage candidate");
            return false;
        }

        if (g_xrefs.front() < g_processChatBounds->begin || g_xrefs.front() >= g_processChatBounds->end)
        {
            Log("resolver rejected inconsistent ProcessChatMessage bounds");
            g_processChatBounds.reset();
            return false;
        }

        g_processChatCalls = EnumerateDirectCalls(*g_processChatBounds, *g_module);
        return !g_processChatCalls.empty();
    }

    void ProbeSTR180()
    {
        FILE* truncate = nullptr;
        fopen_s(&truncate, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "w");
        if (truncate)
            fclose(truncate);

        Log("STRPM bridge resolver target: %s", kTargetBuild);
        Log("source tag: tiltedphoques/TiltedEvolution v1.8.0 (9c23efa422bbc1e5c06eef5522ca73971a513e35)");

        const bool resolved = ResolveProcessChatMessage();
        if (g_module)
        {
            Log("module base=0x%llX size=0x%zX text=0x%llX+0x%zX rdata=0x%llX+0x%zX",
                static_cast<unsigned long long>(g_module->base),
                g_module->size,
                static_cast<unsigned long long>(g_module->textBase),
                g_module->textSize,
                static_cast<unsigned long long>(g_module->rdataBase),
                g_module->rdataSize);
        }

        Log("send-chat anchor copies: %zu", g_anchorAddresses.size());
        for (const auto address : g_anchorAddresses)
            Log("  anchor = 0x%llX (RVA 0x%llX)",
                static_cast<unsigned long long>(address),
                g_module ? static_cast<unsigned long long>(address - g_module->base) : 0ull);

        Log("send-chat RIP xrefs: %zu", g_xrefs.size());
        for (const auto address : g_xrefs)
            Log("  xref = 0x%llX (RVA 0x%llX)",
                static_cast<unsigned long long>(address),
                g_module ? static_cast<unsigned long long>(address - g_module->base) : 0ull);

        if (g_processChatBounds && g_module)
        {
            Log("ProcessChatMessage candidate: 0x%llX-0x%llX (RVA 0x%llX-0x%llX)",
                static_cast<unsigned long long>(g_processChatBounds->begin),
                static_cast<unsigned long long>(g_processChatBounds->end),
                static_cast<unsigned long long>(g_processChatBounds->begin - g_module->base),
                static_cast<unsigned long long>(g_processChatBounds->end - g_module->base));
            Log("direct CALL candidates in ProcessChatMessage: %zu", g_processChatCalls.size());
            for (std::size_t i = 0; i < g_processChatCalls.size(); ++i)
            {
                const auto& call = g_processChatCalls[i];
                Log("  call[%zu] site=0x%llX (RVA 0x%llX) target=0x%llX (RVA 0x%llX)%s",
                    i,
                    static_cast<unsigned long long>(call.site),
                    static_cast<unsigned long long>(call.site - g_module->base),
                    static_cast<unsigned long long>(call.target),
                    static_cast<unsigned long long>(call.target - g_module->base),
                    call.site > g_xrefs.front() ? " [after anchor xref]" : "");
            }
        }

        Log("resolver status: %s", resolved ? "ProcessChatMessage located; transport call still intentionally unarmed" : "not resolved");
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

        // Do not report a working transport until the exact TransportService::Send
        // call target and the receive interception point have both been validated
        // against the public 1.8.0 binary.
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
