# Windows has no rpath: the game is a DLL that a dependency-free launcher loads from bin\.
add_library(sri_game SHARED
    src/main.cpp
    src/GameWindow.cpp
    include/GameWindow.hpp
    include/RuntimeEntry.hpp)
target_include_directories(sri_game PRIVATE include)
target_link_libraries(sri_game PRIVATE Engine::Engine)
set_target_properties(sri_game PROPERTIES DEBUG_POSTFIX "" FOLDER "runtime")
set_project_warnings(sri_game)

# Static CRT: the launcher has to start before bin\ is on the DLL search path.
add_executable(sri_runtime WIN32
    src/Launcher.cpp
    include/RuntimeEntry.hpp)
target_include_directories(sri_runtime PRIVATE include)
set_target_properties(sri_runtime PROPERTIES DEBUG_POSTFIX "" FOLDER "runtime"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_project_warnings(sri_runtime)
add_dependencies(sri_runtime sri_game)

# F5 from the build tree: the renderer compiles shaders from <exe dir>/engine/shaders.
add_custom_command(TARGET sri_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/shaders" "$<TARGET_FILE_DIR:sri_runtime>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/primitives" "$<TARGET_FILE_DIR:sri_runtime>/engine/primitives"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/ui" "$<TARGET_FILE_DIR:sri_runtime>/engine/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/scenes" "$<TARGET_FILE_DIR:sri_runtime>/engine/scenes"
    VERBATIM)

# The portable folder is the template sri-build copies: deploy_sri_runtime.
sri_add_deploy(sri_runtime LIBRARIES engine sri_game LIBRARY_DIR bin)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/shaders" "${PROJECT_SOURCE_DIR}/engine/primitives"
    "${PROJECT_SOURCE_DIR}/engine/ui" "${PROJECT_SOURCE_DIR}/engine/scenes"
    DESTINATION engine COMPONENT sri_runtime)

if(TARGET sri-build)
    add_dependencies(sri-build sri_runtime)
    # Wipe first, like deploy_sri_runtime: stale files would otherwise ship with every game.
    add_custom_target(sri_runtime_template
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:sri-build>/runtime"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SRI_DEPLOY_DIR}/$<CONFIG>/sri_runtime" "$<TARGET_FILE_DIR:sri-build>/runtime"
        COMMENT "Copying the game runtime template beside sri-build" VERBATIM)
    add_dependencies(sri_runtime_template deploy_sri_runtime)
    set_target_properties(sri_runtime_template PROPERTIES FOLDER "runtime")
endif()
