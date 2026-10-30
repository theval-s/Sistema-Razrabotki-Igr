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
find_package(spdlog CONFIG REQUIRED)

# Turns on AUTOMOC / AUTOUIC / AUTORCC and a few sane defaults. Qt 6.3+.
qt_standard_project_setup()

add_subdirectory(core)

qt_add_executable(editor
    src/autoload_dialog.cpp
    src/main.cpp
    src/light_lines.cpp
    src/main_window.cpp
    src/module_loader.cpp
    src/project_dialog.cpp
    src/viewport_widget.cpp
    include/editor/light_lines.hpp
    include/editor/autoload_dialog.hpp
    include/editor/main_window.hpp
    include/editor/module_loader.hpp
    include/editor/project_dialog.hpp
    include/editor/viewport_widget.hpp
)

target_include_directories(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)


target_link_libraries(editor PRIVATE
    editor_core
    ads::qtadvanceddocking-qt6
    Engine::Engine     # brings engine's include dirs along as usage requirements
    # Must be the same shared spdlog the engine uses, so both log into one
    # default logger and the editor's Log dock sees engine messages.
    spdlog::spdlog
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

# Shader paths are resolved beside the executable, just like the game assets.
add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:editor>/engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders" "$<TARGET_FILE_DIR:editor>/engine/shaders"
    VERBATIM
)

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

# --- Engine runtime files -------------------------------------------------
# The renderer compiles its shaders from <exe dir>/engine/shaders at startup;
# engine/primitives holds the built-in meshes and textures (engine::BuiltinAssets).
add_custom_target(EditorAssets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:editor>/engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders"
        "$<TARGET_FILE_DIR:editor>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/primitives"
        "$<TARGET_FILE_DIR:editor>/engine/primitives"
    COMMENT "Copying editor runtime assets"
    VERBATIM
)
add_dependencies(editor EditorAssets)
set_target_properties(EditorAssets PROPERTIES FOLDER "tools")

# --- Editor modules --------------------------------------------------------
# Each module is a DLL in <exe dir>/editor_modules, loaded once at startup
# (see include/editor/editor_module.hpp). Building the editor builds them all.
get_property(SRI_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(SRI_MULTI_CONFIG)
    # A generator expression stops CMake from appending the config directory itself.
    set(SRI_EDITOR_MODULE_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/editor_modules")
else()
    set(SRI_EDITOR_MODULE_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/editor_modules")
endif()

function(sri_add_editor_module name)
    # The loader only picks up editor_module_*.dll: vcpkg puts other DLLs next to the modules.
    if(NOT name MATCHES "^editor_module_")
        message(FATAL_ERROR "Editor module '${name}' must be named editor_module_<something>")
    endif()
    add_library(${name} MODULE ${ARGN})
    target_link_libraries(${name} PRIVATE editor_core)
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        LIBRARY_OUTPUT_DIRECTORY "${SRI_EDITOR_MODULE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${SRI_EDITOR_MODULE_DIR}"
        FOLDER "tools/editor_modules"
    )
    set_project_warnings(${name})
    add_dependencies(editor ${name})
    install(FILES "$<TARGET_FILE:${name}>" DESTINATION editor_modules COMPONENT editor)
endfunction()

add_subdirectory(modules/scene)
add_subdirectory(modules/content)

# --- Install / deploy ------------------------------------------------------

# `--target deploy_editor` assembles a portable editor in
# build/<preset>/deploy/<config>/editor (see cmake/Deploy.cmake).
sri_add_deploy(editor LIBRARIES engine editor_core qtadvanceddocking-qt6)

install(DIRECTORY
        "${PROJECT_SOURCE_DIR}/engine/shaders"
        "${PROJECT_SOURCE_DIR}/engine/primitives"
    DESTINATION engine
    COMPONENT editor
)

# Runs windeployqt at install time: Qt DLLs, plugins and qt.conf. Qt would put
# them in bin/ by default; keep them in the root next to editor.exe instead.
qt_generate_deploy_script(
    TARGET editor
    OUTPUT_SCRIPT deploy_script
    CONTENT "
set(QT_DEPLOY_BIN_DIR .)
set(QT_DEPLOY_LIB_DIR .)
qt_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:editor>\"
    GENERATE_QT_CONF
    DEPLOY_TOOL_OPTIONS --no-opengl-sw
)
")
install(SCRIPT ${deploy_script} COMPONENT editor)

# For building in IDE without running --install
file(GENERATE
        OUTPUT  "$<TARGET_FILE_DIR:editor>/qt.conf"
        CONTENT "[Paths]\nPrefix = ${QT6_INSTALL_PREFIX}\n"
)
