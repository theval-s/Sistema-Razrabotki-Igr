add_executable(sri_runtime WIN32
    src/main.cpp
    src/GameWindow.cpp
    include/GameWindow.hpp)
target_include_directories(sri_runtime PRIVATE include)
target_link_libraries(sri_runtime PRIVATE Engine::Engine)
set_target_properties(sri_runtime PROPERTIES DEBUG_POSTFIX "" FOLDER "runtime")
set_project_warnings(sri_runtime)

# F5 from the build tree: the renderer compiles shaders from <exe dir>/engine/shaders.
add_custom_command(TARGET sri_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/shaders" "$<TARGET_FILE_DIR:sri_runtime>/engine/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/primitives" "$<TARGET_FILE_DIR:sri_runtime>/engine/primitives"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/engine/ui" "$<TARGET_FILE_DIR:sri_runtime>/engine/ui"
    VERBATIM)

# The portable folder is the template sri-build copies: deploy_sri_runtime.
sri_add_deploy(sri_runtime LIBRARIES engine)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/engine/shaders" "${PROJECT_SOURCE_DIR}/engine/primitives"
    "${PROJECT_SOURCE_DIR}/engine/ui"
    DESTINATION engine COMPONENT sri_runtime)

if(TARGET sri-build)
    add_dependencies(sri-build sri_runtime)
    add_custom_target(sri_runtime_template
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SRI_DEPLOY_DIR}/$<CONFIG>/sri_runtime" "$<TARGET_FILE_DIR:sri-build>/runtime"
        COMMENT "Copying the game runtime template beside sri-build" VERBATIM)
    add_dependencies(sri_runtime_template deploy_sri_runtime)
    set_target_properties(sri_runtime_template PROPERTIES FOLDER "runtime")
endif()