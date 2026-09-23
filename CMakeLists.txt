cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# vcpkg has to be picked up BEFORE project() runs, because the toolchain file
# is what teaches find_package() where the manifest-installed packages live.
# ---------------------------------------------------------------------------
include(cmake/Vcpkg.cmake)

project(SRI_Engine
    VERSION 0.1.0
    DESCRIPTION "Sistema Razrabotki Igr"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# --- Language level --------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Options ---------------------------------------------------------------
option(SRI_BUILD_EDITOR "Build the Qt editor" ON)
option(SRI_BUILD_EXAMPLE_GAME "Build the standalone example game" ON)
option(SRI_BUILD_TESTS  "Build tests"         OFF) # We have no tests currently lol

# --- Output layout ---------------------------------------------------------
# Everything runtime-ish lands in one directory so that editor(.exe) sits next
# to Engine.dll / libEngine.so and just runs from the build tree.
# Multi-config generators (VS, Ninja Multi-Config) append /Debug, /Release, ...
# which is fine: the pair still ends up together.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Debug builds get a "d" suffix so debug/release artifacts never overwrite
# each other in an install tree.
set(CMAKE_DEBUG_POSTFIX d)

# Export compile_commands.json for clangd / clang-tidy / IDEs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Folders in Visual Studio
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# --- Subprojects -----------------------------------------------------------
add_subdirectory(external)   # git submodules
add_subdirectory(engine)     # shared library (the SDK)

if(SRI_BUILD_EXAMPLE_GAME)
    add_subdirectory(example_game)
endif()

if(SRI_BUILD_EDITOR)
    add_subdirectory(editor) # Qt app, links against Engine::Engine
endif()

if(SRI_BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)
endif()

# Make "editor" the thing F5 starts in Visual Studio.
if(SRI_BUILD_EDITOR)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTY VS_STARTUP_PROJECT editor)
elseif(SRI_BUILD_EXAMPLE_GAME)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTY VS_STARTUP_PROJECT ExampleGame)
endif()
