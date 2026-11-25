cmake_minimum_required(VERSION 3.16)
project(test_rfb_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RFB_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../mac_mirror_viewer/core_client")
set(RFB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../mac_mirror_viewer/include")
set(WIN_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../mac_mirror_viewer/win_ui")

add_executable(test_rfb_core
    test_rfb_core.cpp
    "${RFB_CORE_DIR}/miniz.c"
    "${RFB_CORE_DIR}/rfb_adaptive.cpp"
    "${RFB_CORE_DIR}/rfb_crypto.cpp"
    "${RFB_CORE_DIR}/rfb_decoder.cpp"
    "${RFB_CORE_DIR}/rfb_session.cpp"
    "${RFB_CORE_DIR}/rfb_transport.cpp"
    "${WIN_UI_DIR}/cli_options.cpp"
)

target_include_directories(test_rfb_core PRIVATE
    "${RFB_INCLUDE_DIR}"
    "${RFB_CORE_DIR}"
    "${WIN_UI_DIR}"
)

if(WIN32)
    target_compile_definitions(test_rfb_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(test_rfb_core PRIVATE ws2_32 bcrypt)
    if(MINGW)
        target_link_options(test_rfb_core PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(test_rfb_core PRIVATE Threads::Threads)
endif()

enable_testing()
add_test(NAME test_rfb_core COMMAND test_rfb_core)
