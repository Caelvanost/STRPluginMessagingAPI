#pragma once

#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

namespace STRPMInventoryBridge
{
    void Start() noexcept;
    void Stop() noexcept;
    bool IsReady() noexcept;
}
