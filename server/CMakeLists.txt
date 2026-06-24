cmake_minimum_required(VERSION 3.16)
set(CMAKE_WARN_DEPRECATED OFF)
project(ns-control CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum OS X deployment version" FORCE)
    add_link_options("-Wl,-w")
endif()

add_compile_options(-O3 -Wall -Wextra)

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

option(USE_UPNP           "Enable UPnP port forwarding via miniupnpc"                           ON)
option(USE_SDL_BT         "Enable local Bluetooth/controller input via SDL3"                    ON)
option(REQUIRE_SDL_BT     "Fail configure if SDL3 Bluetooth/controller support is unavailable"  OFF)
option(USE_BLUEZ_DBUS     "Use BlueZ D-Bus manager for Bluetooth pairing/reconnect on Linux"    ON)
option(REQUIRE_BLUEZ_DBUS "Fail configure if BlueZ D-Bus manager dependencies are unavailable"  ON)
option(BUNDLE_BLUETOOTHD  "Embed a bundled bluetoothd binary into ns-backend"                   OFF)

set(BUNDLED_BLUETOOTHD_PATH "" CACHE STRING "Path to the bluetoothd binary to embed (required when BUNDLE_BLUETOOTHD=ON)")

# Allow passing paths directly (e.g. from cross-compiled miniupnpc)
set(MINIUPNPC_INCLUDE_DIRS "" CACHE STRING "miniupnpc include path")
set(MINIUPNPC_LIBRARIES    "" CACHE STRING "miniupnpc library path")

if(USE_UPNP)
    if(MINIUPNPC_INCLUDE_DIRS AND MINIUPNPC_LIBRARIES)
        add_definitions(-DUSE_UPNP)
        set(MINIUPNPC_FOUND TRUE)
        message(STATUS "UPnP support enabled (external paths: ${MINIUPNPC_INCLUDE_DIRS})")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MINIUPNPC miniupnpc)
        endif()

        if(MINIUPNPC_FOUND)
            add_definitions(-DUSE_UPNP)
            find_library(MINIUPNPC_STATIC_LIBRARY NAMES libminiupnpc.a PATHS ${MINIUPNPC_LIBRARY_DIRS})
            if(MINIUPNPC_STATIC_LIBRARY)
                set(MINIUPNPC_LIBRARIES ${MINIUPNPC_STATIC_LIBRARY})
                message(STATUS "UPnP support enabled (static: ${MINIUPNPC_STATIC_LIBRARY})")
            else()
                message(STATUS "UPnP support enabled (miniupnpc via pkg-config)")
            endif()
        else()
            message(STATUS "UPnP support disabled (miniupnpc not found)")
        endif()
    endif()
else()
    message(STATUS "UPnP support disabled by user")
endif()

if(USE_SDL_BT)
    find_package(SDL3 QUIET CONFIG)
    if(SDL3_FOUND)
        add_definitions(-DNS_ENABLE_SDL_BT)
        message(STATUS "Bluetooth/local controller support enabled (SDL3 found)")
    elseif(REQUIRE_SDL_BT)
        message(FATAL_ERROR "Bluetooth/local controller support requires SDL3, but SDL3 was not found")
    else()
        message(STATUS "Bluetooth/local controller support disabled (SDL3 not found)")
    endif()
else()
    message(STATUS "Bluetooth/local controller support disabled by user")
endif()

if(USE_SDL_BT AND UNIX AND NOT APPLE AND NOT USE_BLUEZ_DBUS)
    message(FATAL_ERROR "Bluetooth/local controller support requires USE_BLUEZ_DBUS=ON; shell fallback was removed")
endif()

if(USE_BLUEZ_DBUS AND UNIX AND NOT APPLE)
    set(NS_SDBUS_CPP_TAG "v2.3.1" CACHE STRING "Pinned sdbus-c++ v2 tag used by the BlueZ manager")

    set(SDBUSCPP_INSTALL          OFF CACHE BOOL "" FORCE)
    set(SDBUSCPP_BUILD_CODEGEN    OFF CACHE BOOL "" FORCE)
    set(SDBUSCPP_BUILD_DOCS       OFF CACHE BOOL "" FORCE)
    set(SDBUSCPP_BUILD_DOXYGEN_DOCS OFF CACHE BOOL "" FORCE)
    set(SDBUSCPP_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
    set(SDBUSCPP_BUILD_TESTS      OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        sdbus_cpp
        GIT_REPOSITORY https://github.com/Kistler-Group/sdbus-cpp.git
        GIT_TAG ${NS_SDBUS_CPP_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sdbus_cpp)

    if(TARGET SDBusCpp::sdbus-c++)
        add_definitions(-DNS_ENABLE_BLUEZ_DBUS)
        set(BLUEZ_DBUS_FOUND TRUE)
        message(STATUS "BlueZ D-Bus manager enabled (pinned sdbus-c++ ${NS_SDBUS_CPP_TAG})")
    elseif(REQUIRE_BLUEZ_DBUS)
        message(FATAL_ERROR "BlueZ D-Bus manager requires pinned sdbus-c++ v2, but the FetchContent target was not created")
    else()
        message(STATUS "BlueZ D-Bus manager disabled (pinned sdbus-c++ target unavailable)")
    endif()
elseif(REQUIRE_BLUEZ_DBUS AND UNIX AND NOT APPLE)
    message(FATAL_ERROR "BlueZ D-Bus manager requires sdbus-c++ on Linux; shell fallback was removed")
else()
    message(STATUS "BlueZ D-Bus manager disabled for this platform")
endif()

find_package(OpenSSL REQUIRED)
find_package(libwebsockets CONFIG)
if(NOT libwebsockets_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LWS REQUIRED libwebsockets)
endif()

find_program(XXD_EXECUTABLE xxd)
if(NOT XXD_EXECUTABLE)
    message(FATAL_ERROR "xxd not found. Install it with: sudo apt install xxd   (or vim-common)")
endif()

# ---------------------------------------------------------------------------
# Embed webapp HTML/CSS/JS files
# ---------------------------------------------------------------------------
set(WEBAPP_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../webapp)
set(WEBAPP_EMBED ${CMAKE_CURRENT_BINARY_DIR}/webapp_embed.h)

add_custom_command(
    OUTPUT ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i index.html    >  ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i mobile.html   >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i editor.html   >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i css/index.css  >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i css/mobile.css >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i css/editor.css >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/bridge.js  >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/index.js   >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/mobile.js  >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/editor.js  >> ${WEBAPP_EMBED}
    DEPENDS
        ${WEBAPP_DIR}/index.html
        ${WEBAPP_DIR}/mobile.html
        ${WEBAPP_DIR}/editor.html
        ${WEBAPP_DIR}/css/index.css
        ${WEBAPP_DIR}/css/mobile.css
        ${WEBAPP_DIR}/css/editor.css
        ${WEBAPP_DIR}/js/bridge.js
        ${WEBAPP_DIR}/js/index.js
        ${WEBAPP_DIR}/js/mobile.js
        ${WEBAPP_DIR}/js/editor.js
    COMMENT "Embedding webapp files via xxd..."
    VERBATIM
)

# ---------------------------------------------------------------------------
# Embed bundled bluetoothd binary (optional)
# ---------------------------------------------------------------------------
if(BUNDLE_BLUETOOTHD)
    if(NOT BUNDLED_BLUETOOTHD_PATH OR NOT EXISTS "${BUNDLED_BLUETOOTHD_PATH}")
        message(FATAL_ERROR
            "BUNDLE_BLUETOOTHD=ON requires a valid BUNDLED_BLUETOOTHD_PATH. "
            "Got: '${BUNDLED_BLUETOOTHD_PATH}'")
    endif()

    # Stage the binary with a fixed filename so xxd produces a predictable
    # C identifier (bluetoothd_staged / bluetoothd_staged_len).
    set(BLUETOOTHD_STAGED ${CMAKE_CURRENT_BINARY_DIR}/bluetoothd_staged)
    set(BLUETOOTHD_EMBED  ${CMAKE_CURRENT_BINARY_DIR}/bluetoothd_embed.h)

    add_custom_command(
        OUTPUT ${BLUETOOTHD_EMBED}
        COMMAND ${CMAKE_COMMAND} -E copy "${BUNDLED_BLUETOOTHD_PATH}" "${BLUETOOTHD_STAGED}"
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                ${XXD_EXECUTABLE} -i bluetoothd_staged > "${BLUETOOTHD_EMBED}"
        DEPENDS "${BUNDLED_BLUETOOTHD_PATH}"
        COMMENT "Embedding bluetoothd ${BUNDLED_BLUETOOTHD_PATH} via xxd..."
        VERBATIM
    )
    add_definitions(-DNS_BUNDLE_BLUETOOTHD)
    message(STATUS "Bundling bluetoothd from: ${BUNDLED_BLUETOOTHD_PATH}")
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(ns-backend
    src/main.cpp
    src/app_state.cpp
    src/web_server.cpp
    src/gadget_wakeup.cpp
    src/virtual_controller.cpp
    src/writers.cpp
    src/bluetooth_input.cpp
    src/bluetooth_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/src/macros.cpp
    ${WEBAPP_EMBED}
)

if(BUNDLE_BLUETOOTHD)
    target_sources(ns-backend PRIVATE ${BLUETOOTHD_EMBED})
endif()

if(SDL3_FOUND)
    target_sources(ns-backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/src/sdl_input.cpp)
endif()

target_include_directories(ns-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ns-backend PRIVATE
    pthread
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(libwebsockets_FOUND)
    target_link_libraries(ns-backend PRIVATE websockets)
else()
    target_include_directories(ns-backend PRIVATE ${LWS_INCLUDE_DIRS})
    target_link_libraries(ns-backend PRIVATE ${LWS_LIBRARIES})
endif()

if(SDL3_FOUND)
    target_link_libraries(ns-backend PRIVATE SDL3::SDL3)
endif()

if(BLUEZ_DBUS_FOUND)
    if(TARGET SDBusCpp::sdbus-c++)
        target_link_libraries(ns-backend PRIVATE SDBusCpp::sdbus-c++)
    elseif(TARGET sdbus-c++)
        target_link_libraries(ns-backend PRIVATE sdbus-c++)
    endif()
endif()

if(MINIUPNPC_FOUND)
    target_include_directories(ns-backend PRIVATE ${MINIUPNPC_INCLUDE_DIRS})
    target_link_libraries(ns-backend PRIVATE ${MINIUPNPC_LIBRARIES})
endif()