cmake_minimum_required(VERSION 3.20)
project(SherwoodEngineFix VERSION 1.1.4 LANGUAGES C CXX)

# Enforce 32-bit (x86) architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SherwoodEngineFix must be compiled as a 32-bit (x86 / Win32) target!")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Fetch MinHook for x86 API & memory function hooking
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

add_library(winmm SHARED
    dllmain.cpp
    proxy_winmm.cpp
    proxy_winmm.def
    config.cpp
    logger.cpp
    pattern.cpp
    window_manager.cpp
    render_ddraw.cpp
    timing.cpp
    widescreen_patch.cpp
    cpu_optimizer.cpp
    memory_opt.cpp
    audio_fix.cpp
    diag.cpp
    input_fix.cpp
    motion_smooth.cpp
    watchdog.cpp
    test_harness.cpp
    ui_scale.cpp
    settings_menu.cpp
    view_zoom.cpp
    ddm/ddm.cpp
    ddm/ddm_blit.cpp
    ddm/ddm_hd.cpp
    ddm/ddm_hd_font.cpp
    ddm/ddm_hd_sprites.cpp
    ddm/ddm_hd_menu.cpp
    ddm/ddm_present.cpp
)

target_include_directories(winmm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${minhook_SOURCE_DIR}/include
)

target_link_libraries(winmm PRIVATE
    minhook
    user32
    gdi32
    ole32
    d3d11
    dxgi
    d3dcompiler
    dxguid
)

target_compile_definitions(winmm PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(winmm PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
)
