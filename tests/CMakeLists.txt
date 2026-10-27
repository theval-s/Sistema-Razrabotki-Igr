find_package(GTest CONFIG REQUIRED)

add_executable(material_assets_tests material_assets_tests.cpp)
target_link_libraries(material_assets_tests PRIVATE Engine::Engine GTest::gtest user32)
set_project_warnings(material_assets_tests)
set_target_properties(material_assets_tests PROPERTIES FOLDER tests)

add_custom_command(TARGET material_assets_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:material_assets_tests> $<TARGET_FILE_DIR:material_assets_tests>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders" "$<TARGET_FILE_DIR:material_assets_tests>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/primitives" "$<TARGET_FILE_DIR:material_assets_tests>/engine/primitives"
    COMMAND_EXPAND_LISTS VERBATIM
)
