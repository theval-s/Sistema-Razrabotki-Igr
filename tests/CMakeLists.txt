find_package(GTest CONFIG REQUIRED)

add_executable(asset_catalog_tests asset_catalog_tests.cpp)
target_link_libraries(asset_catalog_tests PRIVATE Engine::EngineStatic GTest::gtest_main)
set_project_warnings(asset_catalog_tests)

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
