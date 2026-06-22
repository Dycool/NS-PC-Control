cmake_minimum_required(VERSION 3.16)
set(CMAKE_WARN_DEPRECATED OFF)
project(ns-control CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum OS X deployment version" FORCE)
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

option(USE_UPNP "Enable UPnP port forwarding via miniupnpc" ON)
option(USE_SDL_BT "Enable local Bluetooth/controller input via SDL3" ON)
option(REQUIRE_SDL_BT "Fail configure if SDL3 Bluetooth/controller support is unavailable" OFF)

# Allow passing paths directly (e.g. from cross-compiled miniupnpc)
set(MINIUPNPC_INCLUDE_DIRS "" CACHE STRING "miniupnpc include path")
set(MINIUPNPC_LIBRARIES "" CACHE STRING "miniupnpc library path")

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
            # Try to link statically if available, to make the binary more portable.
            find_library(MINIUPNPC_STATIC_LIBRARY NAMES libminiupnpc.a PATHS ${MINIUPNPC_LIBRARY_DIRS})
            if(MINIUPNPC_STATIC_LIBRARY)
                set(MINIUPNPC_LIBRARIES ${MINIUPNPC_STATIC_LIBRARY})
                message(STATUS "UPnP support enabled (using static miniupnpc library: ${MINIUPNPC_STATIC_LIBRARY})")
            else()
                message(STATUS "UPnP support enabled (miniupnpc found via pkg-config)")
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

find_package(OpenSSL REQUIRED)
find_package(libwebsockets CONFIG)
if(NOT libwebsockets_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LWS REQUIRED libwebsockets)
endif()

# Embed webapp HTML files into the binary via xxd
find_program(XXD_EXECUTABLE xxd)
if(NOT XXD_EXECUTABLE)
    message(FATAL_ERROR "xxd not found. Install it with: sudo apt install xxd   (or vim-common)")
endif()

set(WEBAPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../webapp)
set(WEBAPP_EMBED ${CMAKE_CURRENT_BINARY_DIR}/webapp_embed.h)

add_custom_command(
    OUTPUT ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i index.html > ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i mobile.html >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i editor.html >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i css/index.css >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i css/mobile.css >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i css/editor.css >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/bridge.js >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/index.js >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/mobile.js >> ${WEBAPP_EMBED}
    COMMAND ${CMAKE_COMMAND} -E chdir ${WEBAPP_DIR} ${XXD_EXECUTABLE} -i js/editor.js >> ${WEBAPP_EMBED}
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

add_executable(ns-backend
    src/main.cpp
    src/app_state.cpp
    src/web_server.cpp
    src/gadget_wakeup.cpp
    src/virtual_controller.cpp
    src/writers.cpp
    src/bluetooth_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/src/macros.cpp
    ${WEBAPP_EMBED}
)

if(SDL3_FOUND)
    target_sources(ns-backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/src/sdl_input.cpp)
endif()
target_include_directories(ns-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(ns-backend PRIVATE pthread nlohmann_json::nlohmann_json CLI11::CLI11 OpenSSL::SSL OpenSSL::Crypto)
if(libwebsockets_FOUND)
    target_link_libraries(ns-backend PRIVATE websockets)
else()
    target_include_directories(ns-backend PRIVATE ${LWS_INCLUDE_DIRS})
    target_link_libraries(ns-backend PRIVATE ${LWS_LIBRARIES})
endif()

if(SDL3_FOUND)
    target_link_libraries(ns-backend PRIVATE SDL3::SDL3)
endif()

if(MINIUPNPC_FOUND)
    target_include_directories(ns-backend PRIVATE ${MINIUPNPC_INCLUDE_DIRS})
    target_link_libraries(ns-backend PRIVATE ${MINIUPNPC_LIBRARIES})
endif()
