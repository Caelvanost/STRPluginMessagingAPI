#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace STRPMServerModMap
{
    struct Entry
    {
        std::uint16_t id{ 0 };
        bool isLite{ false };
        std::string filename;
    };

    // Copies the latest authenticated STR AuthenticationResponse mod mapping.
    // Returns false until the server has accepted the current connection and the
    // response has been observed by the bridge receive hook.
    bool Snapshot(std::vector<Entry>& outEntries) noexcept;
}
