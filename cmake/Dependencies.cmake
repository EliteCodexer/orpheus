# Dependencies.cmake
# Uses FetchContent to download and build third-party libraries automatically

include(FetchContent)

# Configure FetchContent
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

message(STATUS "Fetching dependencies...")

# ============================================================================
# spdlog - Fast logging library
# ============================================================================
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.13.0
    GIT_SHALLOW    TRUE
)

# ============================================================================
# Zydis - x86/x64 disassembler
# ============================================================================
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
)

# ============================================================================
# Unicorn Engine - CPU Emulator
# ============================================================================
set(UNICORN_ARCH "x86" CACHE STRING "" FORCE)  # Only x86/x64
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    unicorn
    GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
    GIT_TAG        2.1.4
    GIT_SHALLOW    TRUE
)

# ============================================================================
# cpp-httplib (header-only HTTP server library)
# ============================================================================
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
)

# ============================================================================
# nlohmann/json (header-only JSON library)
# ============================================================================
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# ============================================================================
# GoogleTest (for unit tests only)
# ============================================================================
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

# ============================================================================
# Make dependencies available
# ============================================================================
FetchContent_MakeAvailable(
    spdlog
    zydis
    unicorn
    httplib
    json
    googletest
)

# Set include directories for header-only or include-path consumers
set(HTTPLIB_INCLUDE_DIR ${httplib_SOURCE_DIR})
set(JSON_INCLUDE_DIR ${json_SOURCE_DIR}/include)

message(STATUS "All dependencies fetched successfully")
