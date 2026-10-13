#pragma once

#include <cstdint>

namespace STRPMSTRCompatibility
{
    inline constexpr char kTargetBuild[] = "Skyrim Together Reborn 1.8.3";
    inline constexpr char kTargetTag[] = "v1.8.3";
    inline constexpr char kTargetCommit[] = "4917189f6ac382e874f9ee735c4fef6a350e449e";

    // TiltedEvolution v1.8.3 removed kCancelAssignmentRequest from ClientOpcode
    // and kNotifySpawnData from ServerOpcode. The chat opcodes therefore moved
    // down by one compared with the STR 1.8.0 profile used by STRPM 0.9.x.
    inline constexpr std::uint8_t kSendChatMessageRequestOpcode = 37;
    inline constexpr std::uint8_t kNotifyChatMessageBroadcastOpcode = 35;
}
