#pragma once

#include <string_view>

namespace STRPMBridgeReceive
{
    using ChatEnvelopeCallback = void(*)(std::string_view playerName, std::string_view chatMessage);

    bool Start(ChatEnvelopeCallback callback) noexcept;
    void Stop() noexcept;
    bool IsResolved() noexcept;
}
