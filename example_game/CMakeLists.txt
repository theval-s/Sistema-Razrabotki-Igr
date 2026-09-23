add_executable(ExampleGame
    src/main.cpp
)

target_link_libraries(ExampleGame PRIVATE
    Engine::EngineStatic
)

add_custom_target(ExampleGameAssets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:ExampleGame>/engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders"
        "$<TARGET_FILE_DIR:ExampleGame>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:ExampleGame>/models"
    COMMENT "Copying ExampleGame runtime assets"
    VERBATIM
)
add_dependencies(ExampleGame ExampleGameAssets)

set_target_properties(ExampleGame PROPERTIES
    FOLDER "games"
)
set_target_properties(ExampleGameAssets PROPERTIES
    FOLDER "games"
)

set_project_warnings(ExampleGame)
