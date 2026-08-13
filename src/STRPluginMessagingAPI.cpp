#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace STRPM
{
    const Interface* LoadFromModule(const wchar_t* moduleName) noexcept
    {
#if defined(_WIN32)
        if (moduleName == nullptr) {
            return nullptr;
        }

        const auto module = GetModuleHandleW(moduleName);
        if (module == nullptr) {
            return nullptr;
        }

        const auto rawExport =
            GetProcAddress(module, kQueryInterfaceExportName);
        if (rawExport == nullptr) {
            return nullptr;
        }

        const auto queryInterface =
            reinterpret_cast<QueryInterfaceFn>(rawExport);

        const Interface* api = nullptr;
        if (queryInterface(kInterfaceVersion, &api) != Result::kOk) {
            return nullptr;
        }

        if (api == nullptr || api->version != kInterfaceVersion) {
            return nullptr;
        }

        return api;
#else
        (void)moduleName;
        return nullptr;
#endif
    }

    const char* ResultToString(Result result) noexcept
    {
        switch (result) {
        case Result::kOk:
            return "ok";
        case Result::kNotAvailable:
            return "not available";
        case Result::kUnsupportedVersion:
            return "unsupported version";
        case Result::kInvalidArgument:
            return "invalid argument";
        case Result::kNotConnected:
            return "not connected";
        case Result::kChannelAlreadyRegistered:
            return "channel already registered";
        case Result::kChannelNotRegistered:
            return "channel not registered";
        case Result::kPayloadTooLarge:
            return "payload too large";
        case Result::kRateLimited:
            return "rate limited";
        case Result::kTransportError:
            return "transport error";
        default:
            return "unknown result";
        }
    }
}
