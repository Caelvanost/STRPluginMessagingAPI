cmake_minimum_required(VERSION 3.21)

project(
    STRPluginMessagingAPI
    VERSION 0.3.0
    LANGUAGES CXX
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

target_link_libraries(STRPluginMessagingAPIRuntime
    PRIVATE
        Ws2_32
        Bcrypt
)

set_target_properties(STRPluginMessagingAPIRuntime
    PROPERTIES
        OUTPUT_NAME STRPluginMessagingAPI
)

if(MSVC)
    target_compile_options(STRPluginMessagingAPIClient PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(STRPluginMessagingAPIRuntime PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(STRPluginMessagingAPIClient PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(STRPluginMessagingAPIRuntime PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(STRPluginMessagingAPIExample
    examples/ClientUsageExample.cpp
)

target_link_libraries(STRPluginMessagingAPIExample
    PRIVATE
        STRPluginMessagingAPIClient
)
