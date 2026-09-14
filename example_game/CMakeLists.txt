add_executable(ExampleGame
    src/main.cpp
)

target_link_libraries(ExampleGame PRIVATE
    Engine::EngineStatic
)

set_target_properties(ExampleGame PROPERTIES
    FOLDER "games"
)

set_project_warnings(ExampleGame)
