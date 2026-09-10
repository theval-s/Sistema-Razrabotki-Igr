# ---------------------------------------------------------------------------
# editor -- Qt6 application. Uses engine's public headers and links against the
# shared library (Engine.dll / libEngine.so).
#
# Qt is expected to come from an aqt installation. Point CMake at it with one of:
#   -DCMAKE_PREFIX_PATH="C:/Qt/6.10.3/msvc2022_64"
#   -DQt6_DIR="C:/Qt/6.10.3/msvc2022_64/lib/cmake/Qt6"
#   or the QT_ROOT / CMAKE_PREFIX_PATH entries in CMakePresets.json.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Turns on AUTOMOC / AUTOUIC / AUTORCC and a few sane defaults. Qt 6.3+.
qt_standard_project_setup()

qt_add_executable(editor
    src/main.cpp
    src/main_window.cpp
    include/editor/main_window.hpp
)

target_include_directories(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)


target_link_libraries(editor PRIVATE
    Engine::Engine     # brings engine's include dirs along as usage requirements
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(editor PROPERTIES
    # No console window in release builds, but keep one in Debug so stdout /
    # spdlog output is visible while developing.
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    FOLDER "tools"
)

set_project_warnings(editor)

# --- Runtime DLLs (Windows) ------------------------------------------------
# Engine.dll already lands in the same bin/ directory via the global output
# settings; this additionally copies Qt and vcpkg DLLs next to the executable
# so the build tree is runnable without touching PATH.
if(WIN32)
    add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:editor> $<TARGET_FILE_DIR:editor>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

# --- Install / deploy ------------------------------------------------------

# Runs windeployqt at install time so `cmake --install` produces
# a self-contained folder. Requires Qt 6.3+.
qt_generate_deploy_app_script(
    TARGET editor
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# For building in IDE without running --install
file(GENERATE
        OUTPUT  "$<TARGET_FILE_DIR:editor>/qt.conf"
        CONTENT "[Paths]\nPrefix = ${QT6_INSTALL_PREFIX}\n"
)