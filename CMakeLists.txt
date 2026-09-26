cmake_minimum_required(VERSION 3.21)

project(SkyrimVoiceProximity VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# SKSE plugins must match the game's own MSVC ABI (vtables, RTTI, STL type
# layout). Building this with MinGW/g++ produces a DLL with an incompatible
# ABI that will corrupt memory or crash when loaded alongside the MSVC-built
# game and other SKSE plugins. This guard fails the build early instead of
# producing a DLL that appears to build fine and then crashes in-game.
# ---------------------------------------------------------------------------
if (NOT MSVC)
    message(FATAL_ERROR
        "SkyrimVoiceProximity must be built with MSVC. "
        "CommonLibSSE-NG matches the game's MSVC ABI; MinGW/g++ does not. "
        "Use the provided GitHub Actions workflow, or Visual Studio / "
        "MSVC Build Tools locally.")
endif()

# CommonLibSSE-NG, pulled in via vcpkg manifest mode -- see vcpkg.json.
find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/SkyrimVoiceProximity.cpp
        src/IpcServer.cpp
)

# miniaudio.h -- header-only, no linking required, just needs to be on the
# include path. Fetch it first with scripts/fetch-miniaudio.sh.
target_include_directories(${PROJECT_NAME} PRIVATE deps/miniaudio)

target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)

target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Note: `add_commonlibsse_plugin` is the helper macro from the standard
# CommonLibSSE-NG project template. If your fork/template names it
# differently, swap this for a plain add_library(${PROJECT_NAME} SHARED ...)
# plus CommonLibSSE::CommonLibSSE in target_link_libraries.
