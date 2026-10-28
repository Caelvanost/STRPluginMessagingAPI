#pragma once

#include <cstdint>

namespace STRPMSTRCompatibility
{
    inline constexpr char kTargetBuild[] = "Skyrim Together Reborn 1.8.4";
    inline constexpr char kTargetTag[] = "v1.8.4";
    inline constexpr char kTargetCommit[] = "54b3a858e228dc8807ed65bc3f9ac3c3f81b610d";

    // STR 1.8.4 keeps the chat opcode numbering introduced in STR 1.8.3.
    inline constexpr std::uint8_t kSendChatMessageRequestOpcode = 37;
    inline constexpr std::uint8_t kNotifyChatMessageBroadcastOpcode = 35;

    // TiltedConnect EPacketFlags::kReliable. STR 1.8.4 added this explicit
    // TransportService::Send parameter; it is the first enum value, therefore 0.
    inline constexpr int kReliablePacketFlag = 0;
}
