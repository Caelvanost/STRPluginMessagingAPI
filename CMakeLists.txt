cmake_minimum_required(VERSION 3.21)

project(
    STRPluginMessagingAPI
    VERSION 0.1.0
    LANGUAGES CXX
)

add_library(STRPluginMessagingAPI STATIC
    src/STRPluginMessagingAPI.cpp
)

target_compile_features(STRPluginMessagingAPI PUBLIC cxx_std_20)

target_include_directories(STRPluginMessagingAPI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(STRPluginMessagingAPI PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(STRPluginMessagingAPI PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(STRPluginMessagingAPIExample
    examples/ClientUsageExample.cpp
)

target_link_libraries(STRPluginMessagingAPIExample
    PRIVATE
        STRPluginMessagingAPI
)
