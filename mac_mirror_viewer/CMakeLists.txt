cmake_minimum_required(VERSION 3.16)

# Auto-select MinGW-w64 cross-compiler when invoked on macOS/Linux without an explicit toolchain file,
# so `cmake -S mac_mirror_viewer -B mac_mirror_viewer/build` works out of the box!
if(NOT WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
    find_program(MINGW_GXX NAMES x86_64-w64-mingw32-g++ PATHS /opt/homebrew/bin /usr/local/bin /usr/bin)
    find_program(MINGW_GCC NAMES x86_64-w64-mingw32-gcc PATHS /opt/homebrew/bin /usr/local/bin /usr/bin)
    find_program(MINGW_RC  NAMES x86_64-w64-mingw32-windres PATHS /opt/homebrew/bin /usr/local/bin /usr/bin)
    if(MINGW_GXX AND MINGW_GCC)
        set(CMAKE_SYSTEM_NAME Windows)
        set(CMAKE_SYSTEM_PROCESSOR x86_64)
        set(CMAKE_C_COMPILER "${MINGW_GCC}")
        set(CMAKE_CXX_COMPILER "${MINGW_GXX}")
        if(MINGW_RC)
            set(CMAKE_RC_COMPILER "${MINGW_RC}")
        endif()
    endif()
endif()

project(mac_mirror_viewer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Collect core_client sources implemented by @rfb-core
file(GLOB CORE_CLIENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core_client/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core_client/*.c"
)

if(CORE_CLIENT_SOURCES)
    add_library(rfb_core STATIC ${CORE_CLIENT_SOURCES})
    target_include_directories(rfb_core
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/core_client"
    )
    if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rfb_core PRIVATE -Wall -Wextra -O2)
    endif()
    if(WIN32)
        target_compile_definitions(rfb_core PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        )
        target_link_libraries(rfb_core PUBLIC ws2_32 bcrypt)
    endif()
endif()

if(WIN32)
    set(WIN_UI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/win_ui/cli_options.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/win_ui/d3d_renderer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/win_ui/viewer_app.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/win_ui/main_win.cpp"
    )

    add_executable(mac-mirror-viewer WIN32 ${WIN_UI_SOURCES})
    set_target_properties(mac-mirror-viewer PROPERTIES OUTPUT_NAME "mac-mirror-viewer")

    target_include_directories(mac-mirror-viewer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/win_ui"
    )

    target_compile_definitions(mac-mirror-viewer PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )

    if(MINGW)
        target_compile_options(mac-mirror-viewer PRIVATE -Wall -Wextra -O2)
        target_link_options(mac-mirror-viewer PRIVATE
            -mwindows
            -static
            -static-libgcc
            -static-libstdc++
            -s
        )
    endif()

    if(TARGET rfb_core)
        target_link_libraries(mac-mirror-viewer PRIVATE rfb_core)
    endif()

    # Standard built-in Windows SDK system DLLs ONLY (zero external runtime DLLs!)
    target_link_libraries(mac-mirror-viewer PRIVATE
        d3d11
        dxgi
        d2d1
        dwrite
        ws2_32
        bcrypt
        dwmapi
        imm32
        ole32
        shell32
        gdi32
        user32
        uuid
    )
endif()
