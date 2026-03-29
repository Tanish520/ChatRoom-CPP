cmake_minimum_required(VERSION 3.16)

project(ChatRoomCPP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(chat_server
    src/main.cpp
)

target_compile_options(chat_server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(simple_client
    client/simple_client.cpp
)

target_compile_options(simple_client PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
