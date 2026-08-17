cmake_minimum_required(VERSION 3.21)

project(
    STRPluginMessagingAPI
    VERSION 0.4.5
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

    # The STR runtime is unpacked/remapped while Skyrim is starting. A memory span
    # can therefore disappear between VirtualQuery and a later direct read in the
    # resolver. On MSVC, compile a generated copy with one SEH boundary around the
    # complete resolver call. Any transient AV becomes a normal failed probe and
    # the existing 750 ms bootstrap retry can try again once the runtime stabilizes.
    if(MSVC)
        file(READ "${STRPM_BRIDGE_SOURCE}" STRPM_BRIDGE_SOURCE_TEXT)

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

        string(REPLACE
            "${STRPM_BOOTSTRAP_MARKER}"
            "${STRPM_GUARDED_RESOLVER}"
            STRPM_BRIDGE_SOURCE_TEXT
            "${STRPM_BRIDGE_SOURCE_TEXT}"
        )

        set(STRPM_RESOLVER_CALL "if (ResolveProcessChatMessage())")
        string(FIND "${STRPM_BRIDGE_SOURCE_TEXT}" "${STRPM_RESOLVER_CALL}" STRPM_RESOLVER_CALL_POS)
        if(STRPM_RESOLVER_CALL_POS EQUAL -1)
            message(FATAL_ERROR "Unable to locate resolver call in STRPluginMessagingBridge.cpp")
        endif()

        string(REPLACE
            "${STRPM_RESOLVER_CALL}"
            "if (ResolveProcessChatMessageGuarded())"
            STRPM_BRIDGE_SOURCE_TEXT
            "${STRPM_BRIDGE_SOURCE_TEXT}"
        )

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
