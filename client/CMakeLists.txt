cmake_minimum_required(VERSION 3.20)
project(ns_clients LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum OS X deployment version" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenSSL REQUIRED)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11)

set(NS_CLIENT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp")
set(NS_CLIENT_EXTRA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/cli.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dialogs.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/input_settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/macro_client.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/main_window.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/qt_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rumble_client.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/stream_runtime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/udp_protocol.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../server/src/shared/macros.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../server/src/shared/sdl_input.cpp"
)
set(NS_CLIENT_QRC "${CMAKE_CURRENT_LIST_DIR}/resources.qrc")

set(CMAKE_AUTORCC ON)

add_executable(ns-client
    WIN32
    MACOSX_BUNDLE
    "${NS_CLIENT_SOURCE}"
    ${NS_CLIENT_EXTRA_SOURCES}
    "${NS_CLIENT_QRC}"
)

set_target_properties(ns-client PROPERTIES
    AUTORCC ON
)

target_compile_definitions(ns-client PRIVATE
    SDL_MAIN_HANDLED
    NS_CLIENT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
)

target_include_directories(ns-client PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/../server/include"
)

target_link_libraries(ns-client PRIVATE
    Qt6::Widgets
    SDL3::SDL3
    Threads::Threads
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32 AND TARGET Qt6::EntryPoint)
    target_link_libraries(ns-client PRIVATE Qt6::EntryPoint)
endif()

if(WIN32)
    target_sources(ns-client PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/ns-gui.rc"
    )

    target_compile_definitions(ns-client PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
    )

    target_link_libraries(ns-client PRIVATE
        ws2_32
        winmm
    )
endif()

if(WIN32 AND COMMAND qt_import_plugins AND TARGET Qt6::QWindowsIntegrationPlugin)
    qt_import_plugins(ns-client
        INCLUDE_BY_TYPE platforms Qt6::QWindowsIntegrationPlugin
    )
endif()

if(APPLE)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)

    target_link_libraries(ns-client PRIVATE
        ${COREGRAPHICS_FRAMEWORK}
    )

    set_target_properties(ns-client PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "NS PC Control"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.nspccontrol.client"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_INFO_STRING "NS PC Control"
    )
endif()

if(UNIX AND NOT APPLE)
    set_target_properties(ns-client PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(MSVC)
    target_compile_options(ns-client PRIVATE
        /W3
        /permissive-
        /EHsc
    )
else()
    target_compile_options(ns-client PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
