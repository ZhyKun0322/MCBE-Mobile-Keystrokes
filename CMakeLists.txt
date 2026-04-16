cmake_minimum_required(VERSION 3.18)
project(keystrokes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization Flags
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

target_link_libraries(preloader PUBLIC fmt::fmt)

# 3. Setup Include Directories (Matching your Capital 'I' folder)
include_directories(
    ${CMAKE_SOURCE_DIR}/Include
    ${CMAKE_SOURCE_DIR}/Include/ImGui
    ${CMAKE_SOURCE_DIR}/Include/ImGui/backends
    ${CMAKE_SOURCE_DIR}/Include/pl
)

# 4. Set Sources (Matching libkeystrokes_mobile.cpp and Capital 'I' Include)
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

# 5. Link everything
target_link_libraries(keystrokes 
    preloader 
    log 
    android 
    EGL 
    GLESv3 
    GLESv2
)
