cmake_minimum_required(VERSION 3.20)
set(CMAKE_WARN_DEPRECATED OFF)
project(ns_clients LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum OS X deployment version" FORCE)
    add_link_options("-Wl,-w")
endif()

find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenSSL REQUIRED)

if(APPLE)
    function(ns_remove_agl_framework target)
        if(NOT TARGET "${target}")
            return()
        endif()

        foreach(prop IN ITEMS INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
            get_target_property(values "${target}" "${prop}")
            if(NOT values OR values STREQUAL "values-NOTFOUND")
                continue()
            endif()

            set(filtered_values)
            set(pending_framework FALSE)
            foreach(value IN LISTS values)
                if(pending_framework)
                    set(pending_framework FALSE)
                    if(value STREQUAL "AGL")
                        continue()
                    endif()

                    list(APPEND filtered_values "-framework")
                endif()

                if(value STREQUAL "-framework")
                    set(pending_framework TRUE)
                    continue()
                endif()

                if(value STREQUAL "AGL"
                    OR value STREQUAL "-framework AGL"
                    OR value STREQUAL "SHELL:-framework AGL"
                    OR value STREQUAL "LINKER:-framework,AGL")
                    continue()
                endif()

                list(APPEND filtered_values "${value}")
            endforeach()

            if(pending_framework)
                list(APPEND filtered_values "-framework")
            endif()

            set_target_properties("${target}" PROPERTIES "${prop}" "${filtered_values}")
        endforeach()
    endfunction()

    # Qt 6.8.x can still propagate the removed AGL framework through QtGui's
    # OpenGL wrapper. Xcode 26 SDKs no longer ship AGL, and this app does not
    # use the legacy OpenGL entry points that required it.
    ns_remove_agl_framework(Qt6::Gui)
    ns_remove_agl_framework(Qt6::Widgets)
    ns_remove_agl_framework(OpenGL::GL)
    ns_remove_agl_framework(WrapOpenGL::WrapOpenGL)
endif()

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
    GIT_TAG v2.6.2
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
    "${CMAKE_CURRENT_LIST_DIR}/src/mouse_input.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/qt_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rumble_client.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/stream_runtime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/udp_protocol.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../shared/src/macros.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../shared/src/sdl_input.cpp"
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
    "${CMAKE_CURRENT_LIST_DIR}/../shared/include"
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
        shell32
        user32
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
