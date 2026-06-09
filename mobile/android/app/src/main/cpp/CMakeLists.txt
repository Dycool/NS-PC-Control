cmake_minimum_required(VERSION 3.22.1)
project("nsprotocol")

add_library(nsprotocol SHARED
    ns_jni.c
    ../../../../../shared/ns_protocol.c
)

target_include_directories(nsprotocol PRIVATE
    ../../../../../shared/include
)

target_link_libraries(nsprotocol log m)
