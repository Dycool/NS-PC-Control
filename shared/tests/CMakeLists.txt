cmake_minimum_required(VERSION 3.20)
project(ns-tests CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O3 -Wall -Wextra -Wpedantic)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG)

add_definitions(-DNS_ENABLE_SDL_BT)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Unit Tests target — includes server app_state.cpp for deep logic testing
add_executable(ns-unit-tests
    unit_tests.cpp
    test_stubs.cpp
    ../src/macros.cpp
    ../src/sdl_input.cpp
    ../../server/src/app_state.cpp
    ../../server/src/bluetooth_input.cpp
)
target_include_directories(ns-unit-tests PRIVATE
    ../include
    ../../server/include
)
target_link_libraries(ns-unit-tests PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    SDL3::SDL3
)

# Integration Tests target
add_executable(ns-integration-tests
    integration_tests.cpp
    ../src/macros.cpp
)
target_include_directories(ns-integration-tests PRIVATE
    ../include
    ../../server/include
)
target_link_libraries(ns-integration-tests PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
