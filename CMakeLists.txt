cmake_minimum_required(VERSION 3.21)

project(
    STRPluginMessagingAPI
    VERSION 0.4.6
    LANGUAGES CXX
)

option(
    STRPM_ENABLE_UDP_BACKEND
    "Build the legacy standalone UDP backend"
    OFF
)

option(
    STRPM_BUILD_STR180_BRIDGE_PROBE
    "Build the Skyrim Together Reborn 1.8.0 chat bridge"
    ON
)

add_library(STRPluginMessagingAPIClient STATIC
    src/STRPluginMessagingAPI.cpp
)

target_compile_features(STRPluginMessagingAPIClient PUBLIC cxx_std_20)

target_include_directories(STRPluginMessagingAPIClient
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(STRPluginMessagingAPIClient
    PROPERTIES
        OUTPUT_NAME STRPluginMessagingAPIClient
)

add_library(STRPluginMessagingAPIRuntime SHARED
    src/STRPluginMessagingAPIRuntime.cpp
    src/STRPluginMessagingAPISKSEMetadata.cpp
)

target_compile_features(STRPluginMessagingAPIRuntime PUBLIC cxx_std_20)

target_include_directories(STRPluginMessagingAPIRuntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(STRPM_ENABLE_UDP_BACKEND)
    target_compile_definitions(STRPluginMessagingAPIRuntime
        PRIVATE
            STRPM_ENABLE_UDP_BACKEND=1
    )
else()
    target_compile_definitions(STRPluginMessagingAPIRuntime
        PRIVATE
            STRPM_ENABLE_UDP_BACKEND=0
    )
endif()

target_link_libraries(STRPluginMessagingAPIRuntime
    PRIVATE
        Ws2_32
        Bcrypt
)

set_target_properties(STRPluginMessagingAPIRuntime
    PROPERTIES
        OUTPUT_NAME STRPluginMessagingAPI
)

if(STRPM_BUILD_STR180_BRIDGE_PROBE)
    set(STRPM_BRIDGE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/STRPluginMessagingBridge.cpp")

    # STR 1.8.0 unpacks/remaps private runtime regions while Skyrim is starting.
    # Never scan those mappings in-place. For MSVC builds, generate a resolver
    # variant that snapshots candidate spans through ReadProcessMemory and then
    # parses only the local snapshots. A disappearing region therefore becomes a
    # failed resolver pass instead of an access violation in Skyrim.
    if(MSVC)
        file(READ "${STRPM_BRIDGE_SOURCE}" STRPM_BRIDGE_SOURCE_TEXT)

        set(STRPM_FIND_ANCHOR_MARKER "    void FindAnchorCopies()")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_FIND_ANCHOR_MARKER}" STRPM_FIND_ANCHOR_MARKER_POS)
        if(STRPM_FIND_ANCHOR_MARKER_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate FindAnchorCopies in STRPluginMessagingBridge.cpp")
        endif()

        set(STRPM_SAFE_SNAPSHOT_HELPERS [=[    bool SnapshotProcessMemory(
        std::uintptr_t address,
        std::size_t size,
        std::vector<std::uint8_t>& snapshot) noexcept
    {
        constexpr std::size_t kMaxResolverSnapshotBytes = 64u * 1024u * 1024u;
        snapshot.clear();
        if (address == 0 || size == 0 || size > kMaxResolverSnapshotBytes)
            return false;

        try
        {
            snapshot.resize(size);
        }
        catch (...)
        {
            snapshot.clear();
            return false;
        }

        SIZE_T bytesRead = 0;
        const BOOL ok = ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address),
            snapshot.data(),
            size,
            &bytesRead);
        if (!ok || bytesRead != size)
        {
            snapshot.clear();
            return false;
        }
        return true;
    }

    int CompareSnapshotBytes(
        const void* left,
        const void* right,
        std::size_t count) noexcept
    {
        constexpr std::size_t kMaxCompareBytes = 64;
        if (count == 0)
            return 0;
        if (!left || !right || count > kMaxCompareBytes)
            return 1;

        std::uint8_t lhs[kMaxCompareBytes]{};
        std::uint8_t rhs[kMaxCompareBytes]{};
        SIZE_T leftRead = 0;
        SIZE_T rightRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), left, lhs, count, &leftRead) || leftRead != count)
            return 1;
        if (!ReadProcessMemory(GetCurrentProcess(), right, rhs, count, &rightRead) || rightRead != count)
            return 1;

        for (std::size_t i = 0; i < count; ++i)
        {
            if (lhs[i] != rhs[i])
                return lhs[i] < rhs[i] ? -1 : 1;
        }
        return 0;
    }

    void FindAnchorCopies()]=])

        string(REPLACE
            "${STRPM_FIND_ANCHOR_MARKER}"
            "${STRPM_SAFE_SNAPSHOT_HELPERS}"
            STRPM_BRIDGE_SOURCE_TEXT
            "${STRPM_BRIDGE_SOURCE_TEXT}"
        )

        # FindAnchorCopies: copy each readable candidate span before searching it.
        set(STRPM_ANCHOR_DIRECT_READ "            const auto* bytes = reinterpret_cast<const std::uint8_t*>(span.base);\n            for (std::size_t i = 0; i + needleSize <= span.size; ++i)")
        set(STRPM_ANCHOR_SNAPSHOT_READ "            std::vector<std::uint8_t> snapshot;\n            if (!SnapshotProcessMemory(span.base, span.size, snapshot))\n                continue;\n            const auto* bytes = snapshot.data();\n            for (std::size_t i = 0; i + needleSize <= snapshot.size(); ++i)")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_ANCHOR_DIRECT_READ}" STRPM_ANCHOR_DIRECT_READ_POS)
        if(STRPM_ANCHOR_DIRECT_READ_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate anchor direct-read scan")
        endif()
        string(REPLACE "${STRPM_ANCHOR_DIRECT_READ}" "${STRPM_ANCHOR_SNAPSHOT_READ}" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        # FindRipRelativeXrefs: parse executable bytes only from a local snapshot.
        set(STRPM_XREF_DIRECT_READ "            const auto* code = reinterpret_cast<const std::uint8_t*>(span.base);\n            for (std::size_t i = 0; i + 7 <= span.size; ++i)")
        set(STRPM_XREF_SNAPSHOT_READ "            std::vector<std::uint8_t> snapshot;\n            if (!SnapshotProcessMemory(span.base, span.size, snapshot))\n                continue;\n            const auto* code = snapshot.data();\n            for (std::size_t i = 0; i + 7 <= snapshot.size(); ++i)")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_XREF_DIRECT_READ}" STRPM_XREF_DIRECT_READ_POS)
        if(STRPM_XREF_DIRECT_READ_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate xref direct-read scan")
        endif()
        string(REPLACE "${STRPM_XREF_DIRECT_READ}" "${STRPM_XREF_SNAPSHOT_READ}" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        # EnumerateDirectCalls: snapshot the resolved function before opcode parsing.
        set(STRPM_CALL_DIRECT_READ "        const auto* code = reinterpret_cast<const std::uint8_t*>(bounds.begin);\n        const auto size = bounds.end - bounds.begin;")
        set(STRPM_CALL_SNAPSHOT_READ "        const auto size = bounds.end - bounds.begin;\n        std::vector<std::uint8_t> snapshot;\n        if (!SnapshotProcessMemory(bounds.begin, size, snapshot))\n            return result;\n        const auto* code = snapshot.data();")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_CALL_DIRECT_READ}" STRPM_CALL_DIRECT_READ_POS)
        if(STRPM_CALL_DIRECT_READ_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate direct-call memory scan")
        endif()
        string(REPLACE "${STRPM_CALL_DIRECT_READ}" "${STRPM_CALL_SNAPSHOT_READ}" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        # FunctionContainsImmediate32: inspect a local function snapshot only.
        set(STRPM_IMMEDIATE_DIRECT_READ "        const auto* bytes = reinterpret_cast<const std::uint8_t*>(bounds.begin);\n        std::uint8_t needle[sizeof(value)]{};")
        set(STRPM_IMMEDIATE_SNAPSHOT_READ "        std::vector<std::uint8_t> snapshot;\n        if (!SnapshotProcessMemory(bounds.begin, size, snapshot))\n            return false;\n        const auto* bytes = snapshot.data();\n        std::uint8_t needle[sizeof(value)]{};")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_IMMEDIATE_DIRECT_READ}" STRPM_IMMEDIATE_DIRECT_READ_POS)
        if(STRPM_IMMEDIATE_DIRECT_READ_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate immediate-value memory scan")
        endif()
        string(REPLACE "${STRPM_IMMEDIATE_DIRECT_READ}" "${STRPM_IMMEDIATE_SNAPSHOT_READ}" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        # The only remaining memcmp calls now operate on snapshot/local data. Keep
        # them behind ReadProcessMemory as an additional fail-closed boundary.
        string(REPLACE "std::memcmp" "CompareSnapshotBytes" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        # Retain the whole-resolver SEH boundary as a final belt-and-suspenders
        # fallback for Win32 APIs or future resolver code.
        set(STRPM_BOOTSTRAP_MARKER "    void BootstrapLoop()")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_BOOTSTRAP_MARKER}" STRPM_BOOTSTRAP_MARKER_POS)
        if(STRPM_BOOTSTRAP_MARKER_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate BootstrapLoop in STRPluginMessagingBridge.cpp")
        endif()

        set(STRPM_GUARDED_RESOLVER [=[    bool ResolveProcessChatMessageGuarded() noexcept
    {
#if defined(_MSC_VER)
        __try
        {
            return ResolveProcessChatMessage();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        return ResolveProcessChatMessage();
#endif
    }

    void BootstrapLoop()]=])
        string(REPLACE "${STRPM_BOOTSTRAP_MARKER}" "${STRPM_GUARDED_RESOLVER}" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        set(STRPM_RESOLVER_CALL "if (ResolveProcessChatMessage())")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_RESOLVER_CALL}" STRPM_RESOLVER_CALL_POS)
        if(STRPM_RESOLVER_CALL_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate resolver call in STRPluginMessagingBridge.cpp")
        endif()
        string(REPLACE "${STRPM_RESOLVER_CALL}" "if (ResolveProcessChatMessageGuarded())" STRPM_BRIDGE_SOURCE_TEXT "${STRPM_BRIDGE_SOURCE_TEXT}")

        set(STRPM_GENERATED_BRIDGE "${CMAKE_CURRENT_BINARY_DIR}/STRPluginMessagingBridge.guarded.cpp")
        file(WRITE "${STRPM_GENERATED_BRIDGE}" "${STRPM_BRIDGE_SOURCE_TEXT}")
        set(STRPM_BRIDGE_SOURCE "${STRPM_GENERATED_BRIDGE}")
    endif()

    add_library(STRPluginMessagingBridge SHARED
        ${STRPM_BRIDGE_SOURCE}
        src/STRPluginMessagingBridgeReceive.cpp
        src/STRPluginMessagingBridgeSKSE.cpp
    )
    target_compile_features(STRPluginMessagingBridge PUBLIC cxx_std_20)
    target_compile_definitions(STRPluginMessagingBridge
        PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )
    target_include_directories(STRPluginMessagingBridge
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(STRPluginMessagingBridge
        PRIVATE
            Shell32
            Ole32
    )
    set_target_properties(STRPluginMessagingBridge
        PROPERTIES
            OUTPUT_NAME STRPluginMessagingBridge
    )
endif()

if(MSVC)
    target_compile_options(STRPluginMessagingAPIClient PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(STRPluginMessagingAPIRuntime
        PRIVATE
            /W4
            /permissive-
            /EHsc
            /FI${CMAKE_CURRENT_SOURCE_DIR}/src/STRPMRuntimeLoadRedirect.h
    )
    if(STRPM_BUILD_STR180_BRIDGE_PROBE)
        target_compile_options(STRPluginMessagingBridge
            PRIVATE
                /W4
                /permissive-
                /EHsc
                /FI${CMAKE_CURRENT_SOURCE_DIR}/src/STRPMLogRedirect.h
        )
    endif()
else()
    target_compile_options(STRPluginMessagingAPIClient PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(STRPluginMessagingAPIRuntime PRIVATE -Wall -Wextra -Wpedantic)
    if(STRPM_BUILD_STR180_BRIDGE_PROBE)
        target_compile_options(STRPluginMessagingBridge PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

add_executable(STRPluginMessagingAPIExample
    examples/ClientUsageExample.cpp
)

target_link_libraries(STRPluginMessagingAPIExample
    PRIVATE
        STRPluginMessagingAPIClient
)
