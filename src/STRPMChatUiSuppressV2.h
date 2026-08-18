#pragma once

#include "STRPMChatUiSuppress.h"

namespace STRPMChatUiSuppressV2
{
    namespace detail
    {
        using Base = STRPMChatUiSuppress::detail::CandidateBreakpoint;

        // TiltedPhoques::StlAllocator<T> stores one Allocator* on MSVC x64.
        // std::basic_string therefore contains the allocator pointer followed by
        // the ordinary 32-byte MSVC string value.
        struct ShadowString
        {
            void* allocator{ nullptr };
            union
            {
                char small[16];
                const char* heap;
            } storage{};
            std::size_t size{ 0 };
            std::size_t capacity{ 0 };
        };
        static_assert(sizeof(ShadowString) == 40);

        // ServerMessage layout on MSVC x64:
        //   0x00 vptr
        //   0x08 AllocatorCompatible::m_pAllocator
        //   0x10 ServerMessage::m_opcode (uint8) + padding
        // NotifyChatMessageBroadcast:
        //   0x18 MessageType (uint8) + padding
        //   0x20 PlayerName (TiltedPhoques::String, 40 bytes)
        //   0x48 ChatMessage (TiltedPhoques::String, 40 bytes)
        constexpr std::size_t kChatMessageOffset = 0x48;

        inline std::vector<Base> g_candidates;
        inline std::atomic<std::uintptr_t> g_filterAddress{ 0 };
        inline PVOID g_vectoredHandler = nullptr;
        inline thread_local std::uintptr_t g_rearmAddress = 0;

        inline Base* FindCandidate(std::uintptr_t address) noexcept
        {
            for (auto& candidate : g_candidates)
            {
                if (candidate.address == address)
                    return &candidate;
            }
            return nullptr;
        }

        inline bool ReadEnvelopePrefix(const void* messageObject) noexcept
        {
            if (!messageObject)
                return false;

            ShadowString chat{};
            SIZE_T bytesRead = 0;
            const auto stringAddress =
                reinterpret_cast<std::uintptr_t>(messageObject) + kChatMessageOffset;
            if (!ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(stringAddress),
                    &chat,
                    sizeof(chat),
                    &bytesRead) ||
                bytesRead != sizeof(chat))
                return false;

            constexpr auto prefix = STRPMChatUiSuppress::detail::kEnvelopePrefix;
            if (chat.allocator == nullptr ||
                chat.size < prefix.size() ||
                chat.size > 4096 ||
                chat.capacity < chat.size ||
                chat.capacity > (1u << 20))
                return false;

            std::array<char, 16> bytes{};
            if (chat.capacity < 16)
            {
                std::memcpy(bytes.data(), chat.storage.small, prefix.size());
            }
            else
            {
                if (!chat.storage.heap)
                    return false;
                SIZE_T prefixRead = 0;
                if (!ReadProcessMemory(
                        GetCurrentProcess(),
                        chat.storage.heap,
                        bytes.data(),
                        prefix.size(),
                        &prefixRead) ||
                    prefixRead != prefix.size())
                    return false;
            }

            return std::string_view(bytes.data(), prefix.size()) == prefix;
        }

        inline void DisarmOtherCandidates(std::uintptr_t keepAddress) noexcept
        {
            for (auto& candidate : g_candidates)
            {
                if (candidate.address == keepAddress || !candidate.armed)
                    continue;
                if (STRPMChatUiSuppress::detail::PatchByte(
                        candidate.address,
                        candidate.originalByte))
                    candidate.armed = false;
            }
        }

        inline LONG CALLBACK ExceptionHandler(EXCEPTION_POINTERS* exceptionInfo)
        {
            if (!exceptionInfo || !exceptionInfo->ExceptionRecord || !exceptionInfo->ContextRecord)
                return EXCEPTION_CONTINUE_SEARCH;

            const auto code = exceptionInfo->ExceptionRecord->ExceptionCode;
            auto* context = exceptionInfo->ContextRecord;

            if (code == EXCEPTION_SINGLE_STEP && g_rearmAddress != 0)
            {
                if (auto* candidate = FindCandidate(g_rearmAddress);
                    candidate && candidate->armed)
                {
                    STRPMChatUiSuppress::detail::PatchByte(candidate->address, 0xCC);
                }
                g_rearmAddress = 0;
                context->EFlags &= ~0x100u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (code != EXCEPTION_BREAKPOINT)
                return EXCEPTION_CONTINUE_SEARCH;

            const auto address = reinterpret_cast<std::uintptr_t>(
                exceptionInfo->ExceptionRecord->ExceptionAddress);
            auto* candidate = FindCandidate(address);
            if (!candidate || !candidate->armed)
                return EXCEPTION_CONTINUE_SEARCH;

            const bool envelope = ReadEnvelopePrefix(
                reinterpret_cast<const void*>(context->Rdx));
            const auto filterAddress = g_filterAddress.load();
            if (envelope && (filterAddress == 0 || filterAddress == address))
            {
                if (filterAddress == 0)
                {
                    g_filterAddress.store(address);
                    STRPMChatUiSuppress::detail::LogAddress(
                        "STRPM chat UI filter identified OverlayService::OnChatMessageReceived = ",
                        address);
                    DisarmOtherCandidates(address);
                }

                std::uintptr_t returnAddress = 0;
                SIZE_T read = 0;
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(context->Rsp),
                        &returnAddress,
                        sizeof(returnAddress),
                        &read) &&
                    read == sizeof(returnAddress) &&
                    returnAddress != 0)
                {
                    // OnChatMessageReceived returns void. Keep INT3 installed and
                    // emulate RET, suppressing only this STRPM envelope.
                    context->Rsp += sizeof(std::uintptr_t);
                    context->Rip = static_cast<DWORD64>(returnAddress);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            if (!STRPMChatUiSuppress::detail::PatchByte(
                    candidate->address,
                    candidate->originalByte))
                return EXCEPTION_CONTINUE_SEARCH;

            g_rearmAddress = candidate->address;
            context->EFlags |= 0x100u;
            context->Rip = static_cast<DWORD64>(candidate->address);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    inline bool ArmCandidates(const std::vector<std::uintptr_t>& functions)
    {
        if (functions.empty())
            return false;

        if (!detail::g_vectoredHandler)
        {
            detail::g_vectoredHandler = AddVectoredExceptionHandler(
                1,
                &detail::ExceptionHandler);
            if (!detail::g_vectoredHandler)
                return false;
        }

        detail::g_candidates.clear();
        detail::g_candidates.reserve(functions.size());
        for (const auto address : functions)
        {
            std::uint8_t original = 0;
            SIZE_T read = 0;
            if (!ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(address),
                    &original,
                    sizeof(original),
                    &read) ||
                read != sizeof(original))
                continue;

            Base candidate{ address, original, false };
            if (STRPMChatUiSuppress::detail::PatchByte(address, 0xCC))
                candidate.armed = true;
            detail::g_candidates.push_back(candidate);
        }

        return std::any_of(
            detail::g_candidates.begin(),
            detail::g_candidates.end(),
            [](const Base& candidate) { return candidate.armed; });
    }

    inline std::size_t CandidateCount() noexcept
    {
        return detail::g_candidates.size();
    }
}
