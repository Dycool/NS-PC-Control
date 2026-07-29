cmake_minimum_required(VERSION 3.20)
project(ns_pc_control_build_tools LANGUAGES CXX)

add_executable(update-amiibo-catalog update_amiibo_catalog.cpp)
target_compile_features(update-amiibo-catalog PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(update-amiibo-catalog PRIVATE /W4 /permissive-)
else()
    target_compile_options(update-amiibo-catalog PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

install(TARGETS update-amiibo-catalog RUNTIME DESTINATION bin)
