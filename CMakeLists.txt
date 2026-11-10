cmake_minimum_required(VERSION 3.20)
project(LANMessenger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SFML 3 REQUIRED COMPONENTS Graphics Window System)
find_package(Threads REQUIRED)

add_executable(messenger messenger.cpp)
target_link_libraries(messenger PRIVATE
    SFML::Graphics
    SFML::Window
    SFML::System
    Threads::Threads
)

if(WIN32)
    target_link_libraries(messenger PRIVATE ws2_32)
endif()
