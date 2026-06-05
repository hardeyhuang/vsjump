cmake_minimum_required(VERSION 3.15)
project(VsJump CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "VsJump only supports Windows.")
endif()

# Use Unicode Win32 APIs
add_definitions(-DUNICODE -D_UNICODE)

# MSVC-only project (we rely on COM / EnvDTE)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    # Static runtime so the produced exe has no extra DLL deps
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(SOURCES
    src/main.cpp
    src/url_parser.cpp
    src/url_parser.h
    src/protocol_register.cpp
    src/protocol_register.h
    src/vs_locator.cpp
    src/vs_locator.h
    src/picker_dialog.cpp
    src/picker_dialog.h
    src/path_utils.cpp
    src/path_utils.h
    src/vsjump.manifest
)

add_executable(vsjump ${SOURCES})

# Console subsystem with the wide-char CRT entry point (we use wmain).
if(MSVC)
    set_target_properties(vsjump PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup"
    )
endif()

target_include_directories(vsjump PRIVATE src)

# COM / OLE / Shell libs
target_link_libraries(vsjump PRIVATE
    ole32
    oleaut32
    advapi32
    shell32
    user32
    shlwapi
    comctl32
)

install(TARGETS vsjump RUNTIME DESTINATION bin)
