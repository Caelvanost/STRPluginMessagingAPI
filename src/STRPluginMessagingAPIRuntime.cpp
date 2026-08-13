#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct SKSEInterface;

    struct PluginInfo
    {
        std::uint32_t infoVersion;
        const char* name;
        std::uint32_t version;
    };

    struct Listener
    {
        STRPM::ListenerHandle handle{};
        std::string channel;
        STRPM::ReceiveCallback callback{ nullptr };
        void* userData{ nullptr };
    };

    std::mutex g_listenerMutex;
    std::vector<Listener> g_listeners;
    std::atomic_uint64_t g_nextHandle{ 1 };

    std::mutex g_logMutex;
    STRPM::LogCallback g_logCallback{ nullptr };
    void* g_logUserData{ nullptr };

    bool IsValidChannel(const char* channel) noexcept
    {
        if (channel == nullptr || channel[0] == '\0') {
            return false;
        }

        const auto length = std::strlen(channel);
        if (length > STRPM::kMaxChannelLength) {
            return false;
        }

        for (std::size_t index = 0; index < length; ++index) {
            const auto c = static_cast<unsigned char>(channel[index]);
            const bool valid =
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '.' ||
                c == '_' ||
                c == '-';

            if (!valid) {
                return false;
            }
        }

        return true;
    }

    void Log(std::string_view message)
    {
        std::scoped_lock lock(g_logMutex);
        if (g_logCallback != nullptr) {
            g_logCallback(std::string(message).c_str(), g_logUserData);
        }
    }

    STRPM::Result STRPM_CALL RegisterChannel(
        const char* channel,
        STRPM::ReceiveCallback callback,
        void* userData,
        STRPM::ListenerHandle* outHandle)
    {
        if (!IsValidChannel(channel) ||
            callback == nullptr ||
            outHandle == nullptr) {
            return STRPM::Result::kInvalidArgument;
        }

        std::scoped_lock lock(g_listenerMutex);
        const auto alreadyRegistered = std::ranges::any_of(
            g_listeners,
            [channel](const Listener& listener) {
                return listener.channel == channel;
            });

        if (alreadyRegistered) {
            return STRPM::Result::kChannelAlreadyRegistered;
        }

        Listener listener{};
        listener.handle.value = g_nextHandle.fetch_add(1);
        listener.channel = channel;
        listener.callback = callback;
        listener.userData = userData;

        *outHandle = listener.handle;
        g_listeners.push_back(listener);
        return STRPM::Result::kOk;
    }

    STRPM::Result STRPM_CALL UnregisterChannel(
        STRPM::ListenerHandle handle)
    {
        if (handle.value == 0) {
            return STRPM::Result::kInvalidArgument;
        }

        std::scoped_lock lock(g_listenerMutex);
        const auto oldSize = g_listeners.size();
        std::erase_if(g_listeners, [handle](const Listener& listener) {
            return listener.handle.value == handle.value;
        });

        return g_listeners.size() == oldSize ?
            STRPM::Result::kChannelNotRegistered :
            STRPM::Result::kOk;
    }

    STRPM::Result STRPM_CALL Send(
        const char* channel,
        STRPM::Target target,
        const void* data,
        std::size_t size,
        std::uint32_t flags)
    {
        if (!IsValidChannel(channel) || (data == nullptr && size != 0)) {
            return STRPM::Result::kInvalidArgument;
        }

        if (size > STRPM::kMaxPayloadBytes) {
            return STRPM::Result::kPayloadTooLarge;
        }

        if ((flags & STRPM::kMessageAllowLoopback) == 0) {
            Log("STR plugin messaging runtime has no STR transport yet.");
            return STRPM::Result::kNotConnected;
        }

        Listener listener{};
        {
            std::scoped_lock lock(g_listenerMutex);
            const auto it = std::ranges::find_if(
                g_listeners,
                [channel](const Listener& candidate) {
                    return candidate.channel == channel;
                });

            if (it == g_listeners.end()) {
                return STRPM::Result::kChannelNotRegistered;
            }

            listener = *it;
        }

        const STRPM::Message message{
            channel,
            data,
            size,
            STRPM::Sender{ 0, "local", true },
            flags,
            0
        };

        (void)target;
        listener.callback(&message, listener.userData);
        return STRPM::Result::kOk;
    }

    STRPM::Result STRPM_CALL GetLocalConnectionID(
        STRPM::ConnectionID* outConnectionID)
    {
        if (outConnectionID == nullptr) {
            return STRPM::Result::kInvalidArgument;
        }

        *outConnectionID = 0;
        return STRPM::Result::kNotConnected;
    }

    STRPM::Result STRPM_CALL SetLogCallback(
        STRPM::LogCallback callback,
        void* userData)
    {
        std::scoped_lock lock(g_logMutex);
        g_logCallback = callback;
        g_logUserData = userData;
        return STRPM::Result::kOk;
    }

    const STRPM::Interface g_interface{
        STRPM::kInterfaceVersion,
        &RegisterChannel,
        &UnregisterChannel,
        &Send,
        &GetLocalConnectionID,
        &SetLogCallback
    };
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Query(
    const SKSEInterface*,
    PluginInfo* pluginInfo)
{
    if (pluginInfo == nullptr) {
        return false;
    }

    pluginInfo->infoVersion = 1;
    pluginInfo->name = "STRPluginMessagingAPI";
    pluginInfo->version = 1;
    return true;
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(
    const SKSEInterface*)
{
    return true;
}

STRPM_EXPORT STRPM::Result STRPM_CALL STR_QueryPluginMessagingInterface(
    std::uint32_t requestedVersion,
    const STRPM::Interface** outInterface)
{
    if (outInterface == nullptr) {
        return STRPM::Result::kInvalidArgument;
    }

    *outInterface = nullptr;
    if (requestedVersion != STRPM::kInterfaceVersion) {
        return STRPM::Result::kUnsupportedVersion;
    }

    *outInterface = &g_interface;
    return STRPM::Result::kOk;
}
