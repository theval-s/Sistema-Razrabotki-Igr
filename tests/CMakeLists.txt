find_package(GTest CONFIG REQUIRED)

add_executable(audio_tests audio_tests.cpp lua_audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(audio_tests)
set_target_properties(audio_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET audio_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:audio_tests> $<TARGET_FILE_DIR:audio_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME audio_tests COMMAND audio_tests)

add_executable(animation_tests
    animation_tests.cpp
    animation_import_regression_tests.cpp
    animation_scene_tests.cpp
    animation_lua_tests.cpp
    animation_build_tests.cpp
    rig_authoring_tests.cpp
    rig_workflow_integration_tests.cpp
)
add_dependencies(animation_tests sri-build)
target_link_libraries(animation_tests PRIVATE Engine::EngineStatic GTest::gtest_main ozz_animation)
target_include_directories(animation_tests PRIVATE ../engine/src)
target_compile_definitions(animation_tests PRIVATE SRI_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
set_project_warnings(animation_tests)
set_target_properties(animation_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET animation_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:animation_tests> $<TARGET_FILE_DIR:animation_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME animation_tests COMMAND animation_tests)

add_executable(rendering_skinning_tests rendering_skinning_tests.cpp animation_render_tests.cpp)
target_link_libraries(rendering_skinning_tests PRIVATE Engine::EngineStatic GTest::gtest_main d3d11 d3dcompiler dxgi)
target_compile_definitions(rendering_skinning_tests PRIVATE SRI_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
set_project_warnings(rendering_skinning_tests)
set_target_properties(rendering_skinning_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET rendering_skinning_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:rendering_skinning_tests> $<TARGET_FILE_DIR:rendering_skinning_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME rendering_skinning_tests COMMAND rendering_skinning_tests)

foreach(render_target IN ITEMS rendering_diagnostics_tests rendering_resource_tests render_debug_view_tests
        render_debug_native_tests asset_event_tests)
    add_executable(${render_target} ${render_target}.cpp)
    target_link_libraries(${render_target} PRIVATE Engine::EngineStatic GTest::gtest_main d3d11 d3dcompiler dxgi user32)
    target_compile_definitions(${render_target} PRIVATE SRI_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
    set_project_warnings(${render_target})
    set_target_properties(${render_target} PROPERTIES FOLDER tests)
    add_custom_command(TARGET ${render_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${render_target}> $<TARGET_FILE_DIR:${render_target}>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME ${render_target} COMMAND ${render_target})
endforeach()

# Exercise both generated export-header modes against the same utility contract.
foreach(utf_target IN ITEMS utf_tests utf_shared_tests)
    add_executable(${utf_target} utf_tests.cpp)
    if(utf_target STREQUAL "utf_shared_tests")
        target_link_libraries(${utf_target} PRIVATE Engine::Engine GTest::gtest_main)
    else()
        target_link_libraries(${utf_target} PRIVATE Engine::EngineStatic GTest::gtest_main)
    endif()
    set_project_warnings(${utf_target})
    set_target_properties(${utf_target} PROPERTIES FOLDER tests)
    add_custom_command(TARGET ${utf_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${utf_target}> $<TARGET_FILE_DIR:${utf_target}>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME ${utf_target} COMMAND ${utf_target})
endforeach()

add_executable(input_tests input_tests.cpp)
target_link_libraries(input_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(input_tests)
set_target_properties(input_tests PROPERTIES FOLDER tests)
add_test(NAME input_tests COMMAND input_tests)

add_executable(win32_input_tests win32_input_tests.cpp)
target_link_libraries(win32_input_tests PRIVATE Engine::EngineStatic GTest::gtest_main user32)
set_project_warnings(win32_input_tests)
set_target_properties(win32_input_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET win32_input_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:win32_input_tests> $<TARGET_FILE_DIR:win32_input_tests>
    COMMAND_EXPAND_LISTS VERBATIM)
add_test(NAME win32_input_tests COMMAND win32_input_tests)

add_executable(project_tests project_tests.cpp)
target_link_libraries(project_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(project_tests)
set_target_properties(project_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET project_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:project_tests> $<TARGET_FILE_DIR:project_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME project_tests COMMAND project_tests)

if(SRI_BUILD_EDITOR)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
    find_package(spdlog CONFIG REQUIRED)
    add_executable(content_browser_drop_tests
        content_browser_drop_tests.cpp
        content_rig_dialog_tests.cpp
        ../editor/modules/content/content_browser.cpp
        ../editor/modules/content/content_browser.hpp
        ../editor/modules/content/asset_filter_model.cpp
        ../editor/modules/content/asset_filter_model.hpp
        ../editor/modules/content/asset_operations.cpp
        ../editor/modules/content/asset_dialogs.cpp
        ../editor/modules/content/asset_dialogs.hpp
        ../editor/modules/content/scene_placement.cpp
        ../editor/modules/content/rig_dialog.cpp
        ../editor/modules/content/rig_dialog.hpp
        ../editor/modules/content/rig_workflow.cpp
        ../editor/modules/content/rig_workflow.hpp
    )
    target_include_directories(content_browser_drop_tests PRIVATE ../editor/modules/content)
    target_link_libraries(content_browser_drop_tests PRIVATE editor_core GTest::gtest
        Qt6::Test ads::qtadvanceddocking-qt6)
    set_project_warnings(content_browser_drop_tests)
    set_target_properties(content_browser_drop_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET content_browser_drop_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:content_browser_drop_tests> $<TARGET_FILE_DIR:content_browser_drop_tests>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME content_browser_drop_tests COMMAND content_browser_drop_tests)

    add_executable(audio_editor_tests
        audio_editor_tests.cpp
        ../editor/modules/audio/audio_panel.cpp
        ../editor/modules/audio/audio_panel.hpp
    )
    target_include_directories(audio_editor_tests PRIVATE ../editor/modules/audio)
    target_link_libraries(audio_editor_tests PRIVATE editor_core Engine::Engine GTest::gtest
        Qt6::Widgets Qt6::Test ads::qtadvanceddocking-qt6)
    set_project_warnings(audio_editor_tests)
    set_target_properties(audio_editor_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET audio_editor_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:audio_editor_tests> $<TARGET_FILE_DIR:audio_editor_tests>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME audio_editor_tests COMMAND audio_editor_tests)

    add_executable(autoload_dialog_tests
        autoload_dialog_tests.cpp
        ../editor/modules/scripting/autoload_dialog.cpp
        ../editor/modules/scripting/autoload_dialog.hpp
    )
    target_include_directories(autoload_dialog_tests PRIVATE ../editor/modules/scripting)
    target_link_libraries(autoload_dialog_tests PRIVATE editor_core Engine::Engine GTest::gtest
        Qt6::Widgets spdlog::spdlog)
    set_project_warnings(autoload_dialog_tests)
    set_target_properties(autoload_dialog_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET autoload_dialog_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:autoload_dialog_tests> $<TARGET_FILE_DIR:autoload_dialog_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME autoload_dialog_tests COMMAND autoload_dialog_tests)

    add_executable(behavior_tree_graph_tests
        behavior_tree_graph_tests.cpp
        ../editor/modules/ai/parameter_editor.cpp
        ../editor/modules/ai/parameter_editor.hpp
        ../editor/modules/ai/tree_graph.cpp
        ../editor/modules/ai/tree_graph.hpp
        ../editor/modules/ai/tree_panel.cpp
        ../editor/modules/ai/tree_panel.hpp
    )
    target_include_directories(behavior_tree_graph_tests PRIVATE ../editor/modules/ai)
    target_link_libraries(behavior_tree_graph_tests PRIVATE editor_core Engine::Engine GTest::gtest QtNodes::QtNodes
        Qt6::Widgets)
    set_project_warnings(behavior_tree_graph_tests)
    set_target_properties(behavior_tree_graph_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET behavior_tree_graph_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:behavior_tree_graph_tests> $<TARGET_FILE_DIR:behavior_tree_graph_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME behavior_tree_graph_tests COMMAND behavior_tree_graph_tests)

    add_executable(transform_gizmo_tests transform_gizmo_tests.cpp)
    target_link_libraries(transform_gizmo_tests PRIVATE editor_core Engine::Engine GTest::gtest Qt6::Core)
    set_project_warnings(transform_gizmo_tests)
    set_target_properties(transform_gizmo_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET transform_gizmo_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:transform_gizmo_tests> $<TARGET_FILE_DIR:transform_gizmo_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME transform_gizmo_tests COMMAND transform_gizmo_tests)

    add_executable(shortcut_registry_tests shortcut_registry_tests.cpp)
    target_link_libraries(shortcut_registry_tests PRIVATE editor_core GTest::gtest Qt6::Widgets)
    set_project_warnings(shortcut_registry_tests)
    set_target_properties(shortcut_registry_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET shortcut_registry_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:shortcut_registry_tests> $<TARGET_FILE_DIR:shortcut_registry_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME shortcut_registry_tests COMMAND shortcut_registry_tests)

    add_executable(preferences_dialog_tests
        preferences_dialog_tests.cpp
        ../editor/src/preferences_dialog.cpp
        ../editor/include/editor/preferences_dialog.hpp
    )
    target_include_directories(preferences_dialog_tests PRIVATE ../editor/include)
    target_link_libraries(preferences_dialog_tests PRIVATE editor_core GTest::gtest Qt6::Widgets Qt6::Test)
    set_project_warnings(preferences_dialog_tests)
    set_target_properties(preferences_dialog_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET preferences_dialog_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:preferences_dialog_tests> $<TARGET_FILE_DIR:preferences_dialog_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME preferences_dialog_tests COMMAND preferences_dialog_tests)

    add_executable(viewport_input_tests
        viewport_input_tests.cpp
        ../editor/src/viewport_widget.cpp
        ../editor/include/editor/viewport_widget.hpp
    )
    target_include_directories(viewport_input_tests PRIVATE ../editor/include)
    target_link_libraries(viewport_input_tests PRIVATE editor_core Engine::Engine GTest::gtest Qt6::Widgets Qt6::Test)
    set_project_warnings(viewport_input_tests)
    set_target_properties(viewport_input_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET viewport_input_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:viewport_input_tests> $<TARGET_FILE_DIR:viewport_input_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME viewport_input_tests COMMAND viewport_input_tests)

    add_executable(viewport_overlay_tests
        viewport_overlay_tests.cpp
        ../editor/src/viewport_labels.cpp
        ../editor/src/viewport_overlay.cpp
    )
    target_include_directories(viewport_overlay_tests PRIVATE ../editor/include)
    target_link_libraries(viewport_overlay_tests PRIVATE editor_core Engine::Engine GTest::gtest
        Qt6::Widgets Qt6::Test d3d11)
    set_project_warnings(viewport_overlay_tests)
    set_target_properties(viewport_overlay_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET viewport_overlay_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:viewport_overlay_tests> $<TARGET_FILE_DIR:viewport_overlay_tests>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME viewport_overlay_tests COMMAND viewport_overlay_tests)

    add_executable(scene_history_tests scene_history_tests.cpp)
    target_link_libraries(scene_history_tests PRIVATE editor_core Engine::Engine GTest::gtest_main Qt6::Widgets)
    set_project_warnings(scene_history_tests)
    set_target_properties(scene_history_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET scene_history_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:scene_history_tests> $<TARGET_FILE_DIR:scene_history_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME scene_history_tests COMMAND scene_history_tests)

    add_executable(navigation_bake_tests
        navigation_bake_tests.cpp
        ../editor/modules/scene/navigation_bake.cpp
        ../editor/modules/scene/navigation_bake.hpp
    )
    target_include_directories(navigation_bake_tests PRIVATE ../editor/modules/scene)
    target_link_libraries(navigation_bake_tests PRIVATE editor_core Engine::Engine GTest::gtest_main Qt6::Core)
    set_project_warnings(navigation_bake_tests)
    set_target_properties(navigation_bake_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET navigation_bake_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:navigation_bake_tests> $<TARGET_FILE_DIR:navigation_bake_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME navigation_bake_tests COMMAND navigation_bake_tests)

    add_executable(edit_actions_tests edit_actions_tests.cpp)
    target_link_libraries(edit_actions_tests PRIVATE editor_core Engine::Engine GTest::gtest Qt6::Widgets)
    set_project_warnings(edit_actions_tests)
    set_target_properties(edit_actions_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET edit_actions_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:edit_actions_tests> $<TARGET_FILE_DIR:edit_actions_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME edit_actions_tests COMMAND edit_actions_tests)

    add_executable(log_model_tests
        log_model_tests.cpp
        ../editor/src/log_model.cpp
        ../editor/include/editor/log_model.hpp
    )
    target_include_directories(log_model_tests PRIVATE ../editor/include)
    target_link_libraries(log_model_tests PRIVATE GTest::gtest Qt6::Gui spdlog::spdlog)
    set_project_warnings(log_model_tests)
    set_target_properties(log_model_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET log_model_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:log_model_tests> $<TARGET_FILE_DIR:log_model_tests>
        COMMAND_EXPAND_LISTS VERBATIM
    )
    add_test(NAME log_model_tests COMMAND log_model_tests)

    add_executable(editor_rendering_status_tests
        editor_rendering_status_tests.cpp
        ../editor/src/rendering_status.cpp
        ../editor/include/editor/rendering_status.hpp
        ../editor/src/log_model.cpp
        ../editor/include/editor/log_model.hpp
        ../editor/src/log_panel.cpp
        ../editor/include/editor/log_panel.hpp
    )
    target_include_directories(editor_rendering_status_tests PRIVATE ../editor/include)
    target_link_libraries(editor_rendering_status_tests PRIVATE editor_core Engine::Engine
        GTest::gtest Qt6::Widgets spdlog::spdlog user32)
    set_project_warnings(editor_rendering_status_tests)
    set_target_properties(editor_rendering_status_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET editor_rendering_status_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:editor_rendering_status_tests> $<TARGET_FILE_DIR:editor_rendering_status_tests>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME editor_rendering_status_tests COMMAND editor_rendering_status_tests)

    add_executable(editor_rendering_integration_tests
        editor_rendering_integration_tests.cpp
        ../editor/src/log_model.cpp
        ../editor/src/log_panel.cpp
        ../editor/src/main_window.cpp
        ../editor/src/module_loader.cpp
        ../editor/src/preferences_dialog.cpp
        ../editor/src/project_dialog.cpp
        ../editor/src/rendering_status.cpp
        ../editor/src/session_log.cpp
        ../editor/src/viewport_overlay.cpp
        ../editor/src/viewport_labels.cpp
        ../editor/src/viewport_widget.cpp
        ../editor/include/editor/log_model.hpp
        ../editor/include/editor/log_panel.hpp
        ../editor/include/editor/main_window.hpp
        ../editor/include/editor/module_loader.hpp
        ../editor/include/editor/preferences_dialog.hpp
        ../editor/include/editor/project_dialog.hpp
        ../editor/include/editor/viewport_widget.hpp
    )
    target_include_directories(editor_rendering_integration_tests PRIVATE ../editor/include)
    target_compile_definitions(editor_rendering_integration_tests PRIVATE SRI_ENGINE_VERSION="${PROJECT_VERSION}")
    target_link_libraries(editor_rendering_integration_tests PRIVATE editor_core Engine::Engine
        GTest::gtest Qt6::Widgets Qt6::Test spdlog::spdlog ads::qtadvanceddocking-qt6)
    set_project_warnings(editor_rendering_integration_tests)
    set_target_properties(editor_rendering_integration_tests PROPERTIES FOLDER tests AUTOMOC ON)
    add_custom_command(TARGET editor_rendering_integration_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:editor_rendering_integration_tests> $<TARGET_FILE_DIR:editor_rendering_integration_tests>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME editor_rendering_integration_tests COMMAND editor_rendering_integration_tests)

    add_executable(session_log_tests session_log_tests.cpp ../editor/src/session_log.cpp)
    target_include_directories(session_log_tests PRIVATE ../editor/include ../editor/core/include)
    target_compile_definitions(session_log_tests PRIVATE SRI_ENGINE_VERSION="${PROJECT_VERSION}")
    target_link_libraries(session_log_tests PRIVATE Engine::EngineStatic GTest::gtest_main Qt6::Core spdlog::spdlog)
    set_project_warnings(session_log_tests)
    set_target_properties(session_log_tests PROPERTIES FOLDER tests)
    add_custom_command(TARGET session_log_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:session_log_tests> $<TARGET_FILE_DIR:session_log_tests>
        COMMAND_EXPAND_LISTS VERBATIM)
    add_test(NAME session_log_tests COMMAND session_log_tests)
endif()

add_executable(lua_runtime_tests lua_runtime_tests.cpp autoload_session_tests.cpp play_mode_tests.cpp
    runtime_api_tests.cpp gameplay_binding_tests.cpp component_binding_tests.cpp
    persistence_document_tests.cpp persistence_integration_tests.cpp behavior_tree_source_tests.cpp)
target_link_libraries(lua_runtime_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(lua_runtime_tests)
set_target_properties(lua_runtime_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET lua_runtime_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:lua_runtime_tests> $<TARGET_FILE_DIR:lua_runtime_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME lua_runtime_tests COMMAND lua_runtime_tests)

# RmlUi owns process-global interfaces: keep fixtures sequential in one process.
add_executable(ui_tests ui_tests.cpp ui_lifecycle_review_tests.cpp ui_example_tests.cpp ui_text_asset_tests.cpp)
target_link_libraries(ui_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
target_compile_definitions(ui_tests PRIVATE SRI_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_include_directories(ui_tests PRIVATE ../engine/src)
set_project_warnings(ui_tests)
set_target_properties(ui_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET ui_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:ui_tests> $<TARGET_FILE_DIR:ui_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME ui_tests COMMAND ui_tests)
set_tests_properties(ui_tests PROPERTIES RUN_SERIAL TRUE)

find_package(RmlUi 6.3 CONFIG REQUIRED)
add_executable(ui_render_tests ui_render_tests.cpp)
target_link_libraries(ui_render_tests PRIVATE Engine::EngineStatic GTest::gtest_main
    RmlUi::Core user32 d3d11 dxgi ole32 windowscodecs)
target_include_directories(ui_render_tests PRIVATE ../engine/src)
set_project_warnings(ui_render_tests)
set_target_properties(ui_render_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET ui_render_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:ui_render_tests> $<TARGET_FILE_DIR:ui_render_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME ui_render_tests COMMAND ui_render_tests)
set_tests_properties(ui_render_tests PROPERTIES RUN_SERIAL TRUE)

# Explicit developer tool; excluded from normal builds and CTest.
add_executable(ui_benchmark EXCLUDE_FROM_ALL ../tools/ui_benchmark.cpp)
target_link_libraries(ui_benchmark PRIVATE Engine::EngineStatic user32 d3d11 dxgi psapi)
target_include_directories(ui_benchmark PRIVATE ../engine/src)
target_compile_definitions(ui_benchmark PRIVATE SRI_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    SRI_BENCHMARK_CONFIG="$<CONFIG>")
set_project_warnings(ui_benchmark)
set_target_properties(ui_benchmark PROPERTIES FOLDER tools)
add_custom_command(TARGET ui_benchmark POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:ui_benchmark> $<TARGET_FILE_DIR:ui_benchmark>
    COMMAND_EXPAND_LISTS VERBATIM
)

add_executable(asset_catalog_tests asset_catalog_tests.cpp)
target_link_libraries(asset_catalog_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(asset_catalog_tests)
set_target_properties(asset_catalog_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET asset_catalog_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:asset_catalog_tests> $<TARGET_FILE_DIR:asset_catalog_tests>
    COMMAND_EXPAND_LISTS VERBATIM)
add_test(NAME asset_catalog_tests COMMAND asset_catalog_tests)

add_executable(asset_authoring_tests asset_authoring_tests.cpp)
target_link_libraries(asset_authoring_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(asset_authoring_tests)
set_target_properties(asset_authoring_tests PROPERTIES FOLDER tests)

add_executable(navigation_tests navigation_tests.cpp)
target_link_libraries(navigation_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(navigation_tests)
set_target_properties(navigation_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET navigation_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:navigation_tests> $<TARGET_FILE_DIR:navigation_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME navigation_tests COMMAND navigation_tests)

add_executable(file_system_tests file_system_tests.cpp)
target_link_libraries(file_system_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(file_system_tests)
set_target_properties(file_system_tests PROPERTIES FOLDER tests)

add_executable(scene_document_tests scene_document_tests.cpp)
target_link_libraries(scene_document_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
target_compile_definitions(scene_document_tests PRIVATE SRI_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
set_project_warnings(scene_document_tests)
add_custom_command(TARGET scene_document_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/primitives" "$<TARGET_FILE_DIR:scene_document_tests>/engine/primitives"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders" "$<TARGET_FILE_DIR:scene_document_tests>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/models" "$<TARGET_FILE_DIR:scene_document_tests>/game/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/materials" "$<TARGET_FILE_DIR:scene_document_tests>/game/materials"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/shaders" "$<TARGET_FILE_DIR:scene_document_tests>/game/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/scenes" "$<TARGET_FILE_DIR:scene_document_tests>/game/scenes"
    VERBATIM
)

add_executable(material_assets_tests material_assets_tests.cpp)
target_link_libraries(material_assets_tests PRIVATE Engine::Engine GTest::gtest user32)
set_project_warnings(material_assets_tests)
set_target_properties(material_assets_tests PROPERTIES FOLDER tests)

# CPU import tests use the static engine without exporting internal importers.
add_executable(mesh_assets_tests mesh_assets_tests.cpp)
target_link_libraries(mesh_assets_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(mesh_assets_tests)
set_target_properties(mesh_assets_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET mesh_assets_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:mesh_assets_tests> $<TARGET_FILE_DIR:mesh_assets_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)

add_custom_command(TARGET material_assets_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:material_assets_tests> $<TARGET_FILE_DIR:material_assets_tests>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders" "$<TARGET_FILE_DIR:material_assets_tests>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/primitives" "$<TARGET_FILE_DIR:material_assets_tests>/engine/primitives"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/models" "$<TARGET_FILE_DIR:material_assets_tests>/game/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/materials" "$<TARGET_FILE_DIR:material_assets_tests>/game/materials"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/shaders" "$<TARGET_FILE_DIR:material_assets_tests>/game/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/example_game/scenes" "$<TARGET_FILE_DIR:material_assets_tests>/game/scenes"
    COMMAND_EXPAND_LISTS VERBATIM
)
