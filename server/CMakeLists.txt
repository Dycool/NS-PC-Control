cmake_minimum_required(VERSION 3.16)
project(ns-control CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O3 -Wall -Wextra)

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
            message(STATUS "UPnP support enabled (miniupnpc found via pkg-config)")
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
    DEPENDS
        ${WEBAPP_DIR}/index.html
        ${WEBAPP_DIR}/mobile.html
        ${WEBAPP_DIR}/editor.html
    COMMENT "Embedding webapp HTML files via xxd..."
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
    src/upnp.cpp
    src/shared/macros.cpp
    ${WEBAPP_EMBED}
)

if(SDL3_FOUND)
    target_sources(ns-backend PRIVATE src/shared/sdl_input.cpp)
endif()
target_include_directories(ns-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(ns-backend PRIVATE pthread)

if(SDL3_FOUND)
    target_link_libraries(ns-backend PRIVATE SDL3::SDL3)
endif()

if(MINIUPNPC_FOUND)
    target_include_directories(ns-backend PRIVATE ${MINIUPNPC_INCLUDE_DIRS})
    target_link_libraries(ns-backend PRIVATE ${MINIUPNPC_LIBRARIES})
endif()
