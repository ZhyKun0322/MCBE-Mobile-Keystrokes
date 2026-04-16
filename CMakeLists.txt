cmake_minimum_required(VERSION 3.18)
project(keystrokes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization and Obfuscation Flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

include(FetchContent)

# 1. Fetch fmt (Dependency for Preloader)
FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 10.2.1)
FetchContent_MakeAvailable(fmt)

# 2. Fetch Preloader (LiteLDev Official)
FetchContent_Declare(preloader GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git GIT_TAG main)
FetchContent_MakeAvailable(preloader)

# 3. Fetch ImGui (Fixes your "File Not Found" error)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG master)
FetchContent_MakeAvailable(imgui)

# Setup includes
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# 4. Set Sources (Corrected filename to libkeystrokes.cpp)
set(SOURCES
    libkeystrokes.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
)

add_library(keystrokes SHARED ${SOURCES})

# 5. Link everything (Added fmt::fmt and fixed order)
target_link_libraries(keystrokes 
    preloader 
    fmt::fmt 
    log 
    android 
    EGL 
    GLESv3 
    GLESv2
)
