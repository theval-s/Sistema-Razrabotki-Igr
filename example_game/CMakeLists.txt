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
    ${GAME_SHADER_COPY_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ai"
        "$<TARGET_FILE_DIR:ExampleGame>/game/ai"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/materials"
        "$<TARGET_FILE_DIR:ExampleGame>/game/materials"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:ExampleGame>/game/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
        "$<TARGET_FILE_DIR:ExampleGame>/game/scenes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        "$<TARGET_FILE_DIR:ExampleGame>/game/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
        "$<TARGET_FILE_DIR:ExampleGame>/game/scripts"
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

# The standalone binary links the static engine, while vcpkg runtime DLLs are
# collected recursively by the common deploy helper.
sri_add_deploy(ExampleGame)
install(DIRECTORY
    "${PROJECT_SOURCE_DIR}/engine/shaders"
    "${PROJECT_SOURCE_DIR}/engine/primitives"
    "${PROJECT_SOURCE_DIR}/engine/ui"
    DESTINATION engine COMPONENT ExampleGame
)
install(DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${CMAKE_CURRENT_SOURCE_DIR}/materials"
    "${CMAKE_CURRENT_SOURCE_DIR}/models"
    "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    DESTINATION game COMPONENT ExampleGame
)
