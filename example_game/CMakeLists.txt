add_executable(ExampleGame
    src/main.cpp
)

target_link_libraries(ExampleGame PRIVATE
    Engine::EngineStatic
)

set(GAME_SHADER_COPY_COMMAND)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    set(GAME_SHADER_COPY_COMMAND
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:ExampleGame>/game/shaders")
endif()

add_custom_target(ExampleGameAssets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:ExampleGame>/engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/shaders"
        "$<TARGET_FILE_DIR:ExampleGame>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/engine/primitives"
        "$<TARGET_FILE_DIR:ExampleGame>/engine/primitives"
    ${GAME_SHADER_COPY_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/materials"
        "$<TARGET_FILE_DIR:ExampleGame>/game/materials"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:ExampleGame>/game/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
        "$<TARGET_FILE_DIR:ExampleGame>/game/scenes"
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
