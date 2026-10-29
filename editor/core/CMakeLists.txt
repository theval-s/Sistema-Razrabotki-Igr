# ---------------------------------------------------------------------------
# editor_core -- shared library between the editor executable and its modules:
# EditorContext (what a module may touch), the shared Selection and
# SceneDocument, and the IEditorModule interface.
# ---------------------------------------------------------------------------

add_library(editor_core SHARED
    src/editor_context.cpp
    src/scene_document.cpp
    src/selection.cpp
    include/editor/editor_context.hpp
    include/editor/editor_module.hpp
    include/editor/paths.hpp
    include/editor/scene_document.hpp
    include/editor/selection.hpp
)

include(GenerateExportHeader)
generate_export_header(editor_core
    BASE_NAME EDITOR_CORE
    EXPORT_MACRO_NAME EDITOR_CORE_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/editor/editor_core_export.hpp"
)

target_include_directories(editor_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include   # generated export header
)

target_link_libraries(editor_core PUBLIC
    Engine::Engine
    spdlog::spdlog
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
# Private: modules talk to EditorContext only and never see ADS.
target_link_libraries(editor_core PRIVATE ads::qtadvanceddocking-qt6)

set_target_properties(editor_core PROPERTIES
    OUTPUT_NAME editor_core
    FOLDER "tools"
)

set_project_warnings(editor_core)
