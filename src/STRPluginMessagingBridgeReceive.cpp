#include "STRPluginMessagingBridgeReceive.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace STRPMBridgeReceive
{
    namespace
    {
        constexpr char kStructTypeName[] = ".?AUNotifyChatMessageBroadcast@@";
        constexpr char kClassTypeName[] = ".?AVNotifyChatMessageBroadcast@@";

        struct MemorySpan
        {
            std::uintptr_t base{ 0 };
            std::size_t size{ 0 };
            bool readable{ false };
            bool executable{ false };
            void* allocationBase{ nullptr };
            HMODULE allocationModule{ nullptr };
        };

        struct CompleteObjectLocator64
        {
            std::uint32_t signature;
            std::uint32_t offset;
            std::uint32_t cdOffset;
            std::int32_t typeDescriptorRva;
            std::int32_t classDescriptorRva;
            std::int32_t selfRva;
        };

        static_assert(sizeof(CompleteObjectLocator64) == 24);

        struct ShadowBuffer
        {
            void* vtable{ nullptr };
            void* allocator{ nullptr };
            std::uint8_t* data{ nullptr };
            std::size_t size{ 0 };
        };

        struct ShadowReader
        {
            std::size_t bitPosition{ 0 };
            ShadowBuffer* buffer{ nullptr };
        };

        HMODULE g_selfModule = nullptr;
        std::vector<MemorySpan> g_memory;
        ChatEnvelopeCallback g_callback = nullptr;
        std::uintptr_t g_deserializeAddress = 0;
        std::uint8_t g_originalByte = 0;
        std::atomic<bool> g_breakpointArmed{ false };
        PVOID g_vectoredHandler = nullptr;
        thread_local bool g_rearmAfterSingleStep = false;

        void Log(const char* text)
        {
            FILE* file = nullptr;
            fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "a");
            if (!file)
                return;
            std::fprintf(file, "%s\n", text);
            std::fclose(file);
        }

        void LogAddress(const char* label, std::uintptr_t value)
        {
            FILE* file = nullptr;
            fopen_s(&file, "Data\\SKSE\\Plugins\\STRPluginMessagingBridge.log", "a");
            if (!file)
                return;
            std::fprintf(file, "%s0x%llX\n", label, static_cast<unsigned long long>(value));
            std::fclose(file);
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

        HMODULE ModuleForAllocationBase(void* allocationBase)
        {
            if (!allocationBase)
                return nullptr;
            const auto module = reinterpret_cast<HMODULE>(allocationBase);
            char path[MAX_PATH]{};
            if (GetModuleFileNameA(module, path, MAX_PATH) == 0)
                return nullptr;
            return module;
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
                const auto module = ModuleForAllocationBase(mbi.AllocationBase);

                if (mbi.State == MEM_COMMIT && size != 0 && module != g_selfModule)
                {
                    const bool readable = IsReadableProtection(mbi.Protect);
                    const bool executable = IsExecutableProtection(mbi.Protect);
                    if (readable || executable)
                    {
                        g_memory.push_back(MemorySpan{
                            base,
                            size,
                            readable,
                            executable,
                            mbi.AllocationBase,
                            module
                        });
                    }
                }

                if (size == 0 || base + size <= current)
                    break;
                current = base + size;
            }
        }

        bool IsReadableRange(std::uintptr_t address, std::size_t size)
        {
            if (address == 0 || size == 0)
                return false;

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
                return false;
            if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect))
                return false;

            const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto end = begin + static_cast<std::size_t>(mbi.RegionSize);
            return address >= begin && address + size <= end;
        }

        bool IsExecutableAddress(std::uintptr_t address)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
                return false;
            return mbi.State == MEM_COMMIT && IsExecutableProtection(mbi.Protect);
        }

        std::vector<std::uintptr_t> FindAsciiCopies(const char* text)
        {
            std::vector<std::uintptr_t> result;
            const auto length = std::strlen(text);
            if (length == 0)
                return result;

            for (const auto& span : g_memory)
            {
                if (!span.readable || span.size < length)
                    continue;

                const auto* bytes = reinterpret_cast<const std::uint8_t*>(span.base);
                for (std::size_t i = 0; i + length <= span.size; ++i)
                {
                    if (std::memcmp(bytes + i, text, length) == 0)
                        result.push_back(span.base + i);
                }
            }
            return result;
        }

        std::optional<std::uintptr_t> FindTypeDescriptor()
        {
            auto names = FindAsciiCopies(kStructTypeName);
            if (names.empty())
                names = FindAsciiCopies(kClassTypeName);

            std::vector<std::uintptr_t> descriptors;
            for (const auto nameAddress : names)
            {
                if (nameAddress < 16)
                    continue;
                const auto descriptor = nameAddress - 16;
                if (IsReadableRange(descriptor, 16 + std::strlen(kStructTypeName) + 1))
                    descriptors.push_back(descriptor);
            }

            std::sort(descriptors.begin(), descriptors.end());
            descriptors.erase(std::unique(descriptors.begin(), descriptors.end()), descriptors.end());
            if (descriptors.size() != 1)
                return std::nullopt;
            return descriptors.front();
        }

        std::optional<std::uintptr_t> FindCompleteObjectLocator(std::uintptr_t typeDescriptor)
        {
            MEMORY_BASIC_INFORMATION tdMbi{};
            if (VirtualQuery(reinterpret_cast<void*>(typeDescriptor), &tdMbi, sizeof(tdMbi)) != sizeof(tdMbi))
                return std::nullopt;

            const auto expectedImageBase = reinterpret_cast<std::uintptr_t>(tdMbi.AllocationBase);
            std::vector<std::uintptr_t> matches;

            for (const auto& span : g_memory)
            {
                if (!span.readable || reinterpret_cast<std::uintptr_t>(span.allocationBase) != expectedImageBase ||
                    span.size < sizeof(CompleteObjectLocator64))
                {
                    continue;
                }

                for (std::size_t offset = 0; offset + sizeof(CompleteObjectLocator64) <= span.size; offset += 4)
                {
                    const auto address = span.base + offset;
                    const auto* col = reinterpret_cast<const CompleteObjectLocator64*>(address);
                    if (col->signature != 1 || col->selfRva <= 0 || col->typeDescriptorRva <= 0)
                        continue;

                    const auto imageBase = address - static_cast<std::uintptr_t>(col->selfRva);
                    if (imageBase != expectedImageBase)
                        continue;

                    const auto resolvedTypeDescriptor = imageBase + static_cast<std::uintptr_t>(col->typeDescriptorRva);
                    if (resolvedTypeDescriptor == typeDescriptor)
                        matches.push_back(address);
                }
            }

            std::sort(matches.begin(), matches.end());
            matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
            if (matches.size() != 1)
                return std::nullopt;
            return matches.front();
        }

        std::optional<std::uintptr_t> FindVftable(std::uintptr_t completeObjectLocator)
        {
            MEMORY_BASIC_INFORMATION colMbi{};
            if (VirtualQuery(reinterpret_cast<void*>(completeObjectLocator), &colMbi, sizeof(colMbi)) != sizeof(colMbi))
                return std::nullopt;

            const auto expectedAllocationBase = colMbi.AllocationBase;
            std::vector<std::uintptr_t> candidates;

            for (const auto& span : g_memory)
            {
                if (!span.readable || span.allocationBase != expectedAllocationBase || span.size < sizeof(void*))
                    continue;

                for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= span.size; offset += sizeof(void*))
                {
                    const auto slot = span.base + offset;
                    if (*reinterpret_cast<const std::uintptr_t*>(slot) != completeObjectLocator)
                        continue;

                    const auto vftable = slot + sizeof(void*);
                    if (!IsReadableRange(vftable, sizeof(void*) * 5))
                        continue;

                    bool allExecutable = true;
                    for (std::size_t index = 0; index < 5; ++index)
                    {
                        const auto target = *reinterpret_cast<const std::uintptr_t*>(vftable + index * sizeof(void*));
                        if (!IsExecutableAddress(target))
                        {
                            allExecutable = false;
                            break;
                        }
                    }

                    if (allExecutable)
                        candidates.push_back(vftable);
                }
            }

            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            if (candidates.size() != 1)
                return std::nullopt;
            return candidates.front();
        }

        bool ResolveDeserializeAddress()
        {
            EnumerateMemory();

            const auto typeDescriptor = FindTypeDescriptor();
            if (!typeDescriptor)
            {
                Log("receive resolver: NotifyChatMessageBroadcast RTTI type descriptor not uniquely resolved");
                return false;
            }

            const auto completeObjectLocator = FindCompleteObjectLocator(*typeDescriptor);
            if (!completeObjectLocator)
            {
                Log("receive resolver: complete object locator not uniquely resolved");
                return false;
            }

            const auto vftable = FindVftable(*completeObjectLocator);
            if (!vftable)
            {
                Log("receive resolver: vftable not uniquely resolved");
                return false;
            }

            // ServerMessage vftable order on MSVC x64:
            // 0 scalar deleting dtor
            // 1 SerializeRaw
            // 2 SerializeDifferential
            // 3 DeserializeRaw
            // 4 DeserializeDifferential
            const auto deserialize = *reinterpret_cast<const std::uintptr_t*>(
                *vftable + 3 * sizeof(void*));
            if (!IsExecutableAddress(deserialize))
            {
                Log("receive resolver: DeserializeRaw vftable target is not executable");
                return false;
            }

            g_deserializeAddress = deserialize;
            LogAddress("NotifyChatMessageBroadcast::DeserializeRaw = ", g_deserializeAddress);
            return true;
        }

        bool PatchByte(std::uintptr_t address, std::uint8_t value)
        {
            DWORD oldProtect = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &oldProtect))
                return false;

            *reinterpret_cast<volatile std::uint8_t*>(address) = value;
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 1);

            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(address), 1, oldProtect, &ignored);
            return true;
        }

        bool ReadBits(ShadowReader& reader, std::uint64_t& destination, std::size_t count)
        {
            destination = 0;
            if (!reader.buffer || !reader.buffer->data || count > 64)
                return false;

            const auto bitIndex = reader.bitPosition & 0x7;
            std::size_t bitsToRead = 0;
            const auto countOffset = count + bitIndex;
            auto bytesToRead = ((countOffset & ~std::size_t(0x7)) + ((countOffset & 0x7) != 0 ? 8 : 0)) >> 3;
            const auto bytePosition = reader.bitPosition / 8;
            if (bytesToRead + bytePosition > reader.buffer->size)
                return false;

            std::uint64_t endBits = 0;
            auto* location = reader.buffer->data + bytePosition;
            if (bitIndex != 0)
            {
                bitsToRead = 8 - bitIndex;
                if (bitsToRead > count)
                    bitsToRead = count;

                endBits = ((*location) >> bitIndex) & ((std::uint64_t(1) << bitsToRead) - 1);
                ++location;
                --bytesToRead;
            }

            std::copy(location, location + bytesToRead, reinterpret_cast<std::uint8_t*>(&destination));
            destination <<= bitsToRead;
            destination |= endBits;
            if (count < 64)
                destination &= ((std::uint64_t(1) << count) - 1);

            reader.bitPosition += count;
            return true;
        }

        bool ReadBytes(ShadowReader& reader, std::uint8_t* destination, std::size_t count)
        {
            if (!reader.buffer || !reader.buffer->data || (!destination && count != 0))
                return false;

            reader.bitPosition = (reader.bitPosition & ~std::size_t(0x7)) +
                                 ((reader.bitPosition & 0x7) != 0 ? 8 : 0);
            const auto bytePosition = reader.bitPosition / 8;
            if (bytePosition + count > reader.buffer->size)
                return false;

            if (count != 0)
                std::copy(reader.buffer->data + bytePosition, reader.buffer->data + bytePosition + count, destination);
            reader.bitPosition += count * 8;
            return true;
        }

        bool ReadVarInt(ShadowReader& reader, std::uint64_t& value)
        {
            value = 0;
            std::uint32_t shift = 0;
            while (shift < 64)
            {
                std::uint64_t chunk = 0;
                std::uint64_t more = 0;
                if (!ReadBits(reader, chunk, 7) || !ReadBits(reader, more, 1))
                    return false;
                value |= chunk << shift;
                shift += 7;
                if (more == 0)
                    return true;
            }
            return false;
        }

        bool ReadString(ShadowReader& reader, std::string& value)
        {
            std::uint64_t rawLength = 0;
            if (!ReadVarInt(reader, rawLength))
                return false;

            const auto length = static_cast<std::uint16_t>(rawLength & 0xFFFF);
            value.resize(length);
            if (length == 0)
                return true;
            return ReadBytes(reader, reinterpret_cast<std::uint8_t*>(value.data()), length);
        }

        bool PeekChatEnvelope(ShadowReader* originalReader, std::string& playerName, std::string& chatMessage)
        {
            if (!originalReader || !originalReader->buffer)
                return false;

            ShadowReader reader = *originalReader;
            std::uint64_t messageType = 0;
            if (!ReadVarInt(reader, messageType))
                return false;
            if (!ReadString(reader, playerName) || !ReadString(reader, chatMessage))
                return false;

            return chatMessage.rfind("STRPM|v2|", 0) == 0;
        }

        LONG CALLBACK ReceiveExceptionHandler(EXCEPTION_POINTERS* exceptionInfo)
        {
            if (!exceptionInfo || !exceptionInfo->ExceptionRecord || !exceptionInfo->ContextRecord)
                return EXCEPTION_CONTINUE_SEARCH;

            const auto code = exceptionInfo->ExceptionRecord->ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP && g_rearmAfterSingleStep)
            {
                if (g_breakpointArmed.load())
                    PatchByte(g_deserializeAddress, 0xCC);
                g_rearmAfterSingleStep = false;
                exceptionInfo->ContextRecord->EFlags &= ~0x100u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (code != EXCEPTION_BREAKPOINT ||
                reinterpret_cast<std::uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress) != g_deserializeAddress ||
                !g_breakpointArmed.load())
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            auto* reader = reinterpret_cast<ShadowReader*>(exceptionInfo->ContextRecord->Rdx);
            std::string playerName;
            std::string chatMessage;
            if (PeekChatEnvelope(reader, playerName, chatMessage) && g_callback)
                g_callback(playerName, chatMessage);

            // Let STR execute DeserializeRaw normally, then restore the breakpoint
            // after exactly one instruction. Suppression from the visible CEF chat
            // is handled at a later hook point; this stage establishes reliable
            // receive callbacks first.
            PatchByte(g_deserializeAddress, g_originalByte);
            g_rearmAfterSingleStep = true;
            exceptionInfo->ContextRecord->EFlags |= 0x100u;
            exceptionInfo->ContextRecord->Rip = static_cast<DWORD64>(g_deserializeAddress);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        bool ArmBreakpoint()
        {
            if (g_deserializeAddress == 0 || g_breakpointArmed.load())
                return false;

            if (!g_vectoredHandler)
            {
                g_vectoredHandler = AddVectoredExceptionHandler(1, &ReceiveExceptionHandler);
                if (!g_vectoredHandler)
                    return false;
            }

            g_originalByte = *reinterpret_cast<const std::uint8_t*>(g_deserializeAddress);
            g_breakpointArmed.store(true);
            if (!PatchByte(g_deserializeAddress, 0xCC))
            {
                g_breakpointArmed.store(false);
                return false;
            }
            return true;
        }
    }

    bool Start(ChatEnvelopeCallback callback) noexcept
    {
        g_callback = callback;
        if (!ResolveDeserializeAddress())
            return false;
        if (!ArmBreakpoint())
        {
            Log("receive resolver: failed to arm DeserializeRaw breakpoint");
            return false;
        }
        Log("receive breakpoint armed for NotifyChatMessageBroadcast::DeserializeRaw");
        return true;
    }

    void Stop() noexcept
    {
        if (g_breakpointArmed.exchange(false) && g_deserializeAddress != 0)
            PatchByte(g_deserializeAddress, g_originalByte);

        if (g_vectoredHandler)
        {
            RemoveVectoredExceptionHandler(g_vectoredHandler);
            g_vectoredHandler = nullptr;
        }

        g_callback = nullptr;
        g_deserializeAddress = 0;
        g_rearmAfterSingleStep = false;
    }

    bool IsResolved() noexcept
    {
        return g_deserializeAddress != 0 && g_breakpointArmed.load();
    }
}
