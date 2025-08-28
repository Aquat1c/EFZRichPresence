cmake_minimum_required(VERSION 3.20)
project(EfzRichPresence VERSION 0.1.0 LANGUAGES CXX)

# Windows-only
if(NOT WIN32)
    message(FATAL_ERROR "EfzRichPresence builds only on Windows.")
endif()

# Options
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Sources
file(GLOB_RECURSE SOURCES
    src/*.cpp
)
file(GLOB_RECURSE HEADERS
    include/**/*.h
)

add_library(EfzRichPresence SHARED ${SOURCES} ${HEADERS})

# MSVC warnings & defines
if(MSVC)
    target_compile_options(EfzRichPresence PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(EfzRichPresence PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN _WINSOCKAPI_)
    # Link common Win32 libs like other EFZ mods
    target_link_libraries(EfzRichPresence PRIVATE Ole32 user32 kernel32 psapi)
endif()

# Include dirs
 target_include_directories(EfzRichPresence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Define a macro for version
 target_compile_definitions(EfzRichPresence PRIVATE EFZDA_VERSION=\"${PROJECT_VERSION}\")

# Optional: embed a default Discord App ID here to avoid requiring a file.
# Replace the placeholder with your actual Application (Client) ID from Discord Developer Portal.
# Example: -DEFZDA_DEFAULT_APP_ID=\"123456789012345678\"
if (NOT DEFINED EFZDA_DEFAULT_APP_ID)
    # Embedded default Discord Application ID (can be overridden at configure time)
    set(EFZDA_DEFAULT_APP_ID "1410635317765210204")
endif()
target_compile_definitions(EfzRichPresence PRIVATE EFZDA_DEFAULT_APP_ID="${EFZDA_DEFAULT_APP_ID}")

# Set subsystem for DLL
set_target_properties(EfzRichPresence PROPERTIES
    # IMPORTANT: many EFZ loaders scan for DLLs prefixed with "efz_"
    OUTPUT_NAME "EfzRichPresence"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# Prefer static runtime to align with other EFZ mods
if (MSVC)
    set_property(TARGET EfzRichPresence PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Console window option to match other mods
option(EFZ_ENABLE_CONSOLE "Enable debug console window" ON)
if (EFZ_ENABLE_CONSOLE)
    target_compile_definitions(EfzRichPresence PRIVATE EFZ_ENABLE_CONSOLE)
endif()

# Install (optional)
install(TARGETS EfzRichPresence RUNTIME DESTINATION . LIBRARY DESTINATION .)
