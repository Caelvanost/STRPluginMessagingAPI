cmake_minimum_required(VERSION 3.21)

project(
    STRPluginMessagingAPI
    VERSION 0.4.0
    LANGUAGES CXX
)

option(
    STRPM_ENABLE_UDP_BACKEND
    "Build the legacy standalone UDP backend"
    OFF
)

option(
    STRPM_BUILD_STR180_BRIDGE_PROBE
    "Build the Skyrim Together Reborn 1.8.0 chat bridge probe"
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
)

target_compile_features(STRPluginMessagingAPIRuntime PUBLIC cxx_std_20)

target_include_directories(STRPluginMessagingAPIRuntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
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
    add_library(STRPluginMessagingBridge SHARED
        src/STRPluginMessagingBridge.cpp
    )
    target_compile_features(STRPluginMessagingBridge PUBLIC cxx_std_20)
    target_include_directories(STRPluginMessagingBridge
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    set_target_properties(STRPluginMessagingBridge
        PROPERTIES
            OUTPUT_NAME STRPluginMessagingBridge
    )
endif()

if(MSVC)
    target_compile_options(STRPluginMessagingAPIClient PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(STRPluginMessagingAPIRuntime PRIVATE /W4 /permissive- /EHsc)
    if(STRPM_BUILD_STR180_BRIDGE_PROBE)
        target_compile_options(STRPluginMessagingBridge PRIVATE /W4 /permissive- /EHsc)
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
