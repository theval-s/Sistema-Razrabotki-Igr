find_package(GTest CONFIG REQUIRED)

add_executable(input_tests input_tests.cpp)
target_link_libraries(input_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(input_tests)
set_target_properties(input_tests PROPERTIES FOLDER tests)

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
endif()

add_executable(lua_runtime_tests lua_runtime_tests.cpp autoload_session_tests.cpp play_mode_tests.cpp
    runtime_api_tests.cpp gameplay_binding_tests.cpp component_binding_tests.cpp
    persistence_document_tests.cpp persistence_integration_tests.cpp)
target_link_libraries(lua_runtime_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(lua_runtime_tests)
set_target_properties(lua_runtime_tests PROPERTIES FOLDER tests)
add_custom_command(TARGET lua_runtime_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:lua_runtime_tests> $<TARGET_FILE_DIR:lua_runtime_tests>
    COMMAND_EXPAND_LISTS VERBATIM
)
add_test(NAME lua_runtime_tests COMMAND lua_runtime_tests)

add_executable(asset_catalog_tests asset_catalog_tests.cpp)
target_link_libraries(asset_catalog_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(asset_catalog_tests)

add_executable(asset_authoring_tests asset_authoring_tests.cpp)
target_link_libraries(asset_authoring_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(asset_authoring_tests)
set_target_properties(asset_authoring_tests PROPERTIES FOLDER tests)

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
