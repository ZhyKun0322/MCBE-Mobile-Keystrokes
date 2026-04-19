cmake_minimum_required(VERSION 3.18)
project(keystrokes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

include(FetchContent)

# 1. fmt  (required by preloader)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1)
FetchContent_MakeAvailable(fmt)

# 2. Preloader  (provides pl/Hook.h and pl/Gloss.h)
FetchContent_Declare(preloader
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG main)
FetchContent_MakeAvailable(preloader)
target_link_libraries(preloader PUBLIC fmt::fmt)

# 3. entt  (header-only, provides entt/entt.hpp)
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.0)
FetchContent_MakeAvailable(entt)

# 4. Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/Include
    ${CMAKE_SOURCE_DIR}/Include/ImGui
    ${CMAKE_SOURCE_DIR}/Include/ImGui/backends
    ${CMAKE_SOURCE_DIR}/Include/pl
    ${entt_SOURCE_DIR}/single_include   # provides entt/entt.hpp
)

# 5. Sources
set(SOURCES
    libkeystrokes_mobile.cpp
    Include/ImGui/imgui.cpp
    Include/ImGui/imgui_draw.cpp
    Include/ImGui/imgui_tables.cpp
    Include/ImGui/imgui_widgets.cpp
    Include/ImGui/backends/imgui_impl_opengl3.cpp
    Include/ImGui/backends/imgui_impl_android.cpp
)

add_library(keystrokes SHARED ${SOURCES})

# 6. Link
target_link_libraries(keystrokes
    preloader
    EnTT::EnTT
    log
    android
    EGL
    GLESv3
    GLESv2
)
