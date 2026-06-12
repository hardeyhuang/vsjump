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
    src/everything_search.cpp
    src/everything_search.h
    src/vsjump.manifest
)

add_executable(vsjump ${SOURCES})

# Windows (GUI) subsystem so that launching vsjump.exe from the shell or a
# browser protocol handler does NOT flash a black console window. We still
# want wide-char argv, so use the wWinMain CRT entry point.
if(MSVC)
    set_target_properties(vsjump PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup"
    )
endif()

target_include_directories(vsjump PRIVATE src)

# Everything SDK (vendored): we only need the header at compile time; the
# DLL is loaded lazily at runtime via LoadLibrary, so no .lib link is needed.
set(EVERYTHING_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/everything-sdk")
target_include_directories(vsjump PRIVATE "${EVERYTHING_SDK_DIR}/include")

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

# Stage Everything64.dll next to vsjump.exe so the runtime LoadLibrary call
# finds it without polluting PATH.
set(EVERYTHING_DLL "${EVERYTHING_SDK_DIR}/dll/Everything64.dll")
if(EXISTS "${EVERYTHING_DLL}")
    add_custom_command(TARGET vsjump POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EVERYTHING_DLL}"
                "$<TARGET_FILE_DIR:vsjump>/Everything64.dll"
        COMMENT "Copying Everything64.dll next to vsjump.exe"
        VERBATIM)
endif()

install(TARGETS vsjump RUNTIME DESTINATION bin)
if(EXISTS "${EVERYTHING_DLL}")
    install(FILES "${EVERYTHING_DLL}" DESTINATION bin)
endif()
