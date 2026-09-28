# ---------------------------------------------------------------------------
# engine -- the SDK. Built as a shared library; the editor links against it.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)

set(ENGINE_PUBLIC_HEADERS
    include/engine/BindSlot.hpp
    include/engine/Buffer.hpp
    include/engine/engine.hpp
    include/engine/FPSCamera.hpp
    include/engine/GBufferPass.hpp
    include/engine/GraphicalState.hpp
    include/engine/HandleMap.hpp
    include/engine/LightPass.hpp
    include/engine/PresentPass.hpp
    include/engine/RenderedMesh.h
    include/engine/RenderingContext.hpp
    include/engine/RenderingResourceManager.hpp
    include/engine/RenderingResources.h
    include/engine/RenderingSystem.hpp
    include/engine/RenderPass.hpp
    include/engine/RenderWorld.hpp
    include/engine/ResourceID.hpp
    include/engine/ShaderContext.hpp
    include/engine/ShadowPass.h
    include/engine/Texture.hpp
    include/engine/UglyUtils.hpp
    include/engine/VertexLayout.hpp
    include/engine/VertexLayoutCache.hpp
    include/engine/window.hpp
)

set(ENGINE_SOURCES
    src/BindSlot.cpp
    src/Buffer.cpp
    src/engine.cpp
    src/FPSCamera.cpp
    src/GBufferPass.cpp
    src/GraphicalState.cpp
    src/HandleMap.cpp
    src/LightPass.cpp
    src/PresentPass.cpp
    src/RenderedMesh.cpp
    src/RenderingContext.cpp
    src/RenderingResourceManager.cpp
    src/RenderingResources.cpp
    src/RenderingSystem.cpp
    src/RenderPass.cpp
    src/RenderWorld.cpp
    src/ResourceID.cpp
    src/ShaderContext.cpp
    src/ShadowPass.cpp
    src/Texture.cpp
    src/UglyUtils.cpp
    src/VertexLayout.cpp
    src/VertexLayoutCache.cpp
    src/window.cpp
)

set(ENGINE_SHADERS
    shaders/Common.hlsl
    shaders/DirectionalLightVS.hlsl
    shaders/GBufferPass.hlsl
    shaders/PresentPass.hlsl
    shaders/LightPass.hlsl
    shaders/ShadowCSMPass.hlsl
)

# Keep shaders visible in IDE projects without invoking Visual Studio's HLSL compiler.
set_source_files_properties(${ENGINE_SHADERS} PROPERTIES HEADER_FILE_ONLY ON)
source_group("Shaders" FILES ${ENGINE_SHADERS})

add_library(engine SHARED
    ${ENGINE_SOURCES}
    ${ENGINE_PUBLIC_HEADERS}
    ${ENGINE_SHADERS}
)
add_library(Engine::Engine ALIAS engine)

add_library(engine_static STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_PUBLIC_HEADERS}
    ${ENGINE_SHADERS}
)
add_library(Engine::EngineStatic ALIAS engine_static)

# --- Export macros ---------------------------------------------------------
# Generates ENGINE_API (dllexport when building the dll, dllimport when
# consuming it) plus ENGINE_DEPRECATED etc. Nothing to maintain by hand.
include(GenerateExportHeader)
generate_export_header(engine
    BASE_NAME ENGINE
    EXPORT_MACRO_NAME ENGINE_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/engine/engine_export.hpp"
)

target_include_directories(engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>   # generated export header
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(engine_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The generated header makes ENGINE_API empty for static consumers.
target_compile_definitions(engine_static PUBLIC ENGINE_STATIC_DEFINE)

# PRIVATE where the type does not appear in a public header -- that way the
# editor does not have to find_package() our implementation details.
target_link_libraries(engine
    PUBLIC
        spdlog::spdlog Microsoft::DirectXTK
    PRIVATE
        d3d11 d3dcompiler dxgi dxguid
)

target_link_libraries(engine_static
    PUBLIC
        spdlog::spdlog Microsoft::DirectXTK
    PRIVATE
        d3d11 d3dcompiler dxgi dxguid
)

set_target_properties(engine PROPERTIES
    OUTPUT_NAME            Engine
    VERSION                ${PROJECT_VERSION}
    SOVERSION              ${PROJECT_VERSION_MAJOR}
    # Should be OFF so we only export specific symbols, but can set to ON for dev
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    FOLDER                 "engine"
)

set_target_properties(engine_static PROPERTIES
    OUTPUT_NAME EngineStatic
    FOLDER      "engine"
)

set_project_warnings(engine)
set_project_warnings(engine_static)

# --- Install (SDK layout) --------------------------------------------------
include(GNUInstallDirs)

install(TARGETS engine
    EXPORT  EngineTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/engine
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/engine/engine_export.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/engine
)

install(EXPORT EngineTargets
    FILE      EngineTargets.cmake
    NAMESPACE Engine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Engine
)

# --- Core kernel -------------------------------------------------------------
# Engine lifecycle, subsystems, events and the scene model -- the API
# every other system (rendering, input, audio, editor, ...) builds on.

find_package(EnTT CONFIG REQUIRED)
find_package(enkiTS CONFIG REQUIRED)

set(ENGINE_CORE_HEADERS
    include/engine/core/CoreEvents.hpp
    include/engine/core/FrameContext.hpp
    include/engine/core/Subsystem.hpp
    include/engine/core/Surface.hpp
    include/engine/scene/Components.hpp
    include/engine/scene/Scene.hpp
    include/engine/scene/Transform.hpp
)

set(ENGINE_CORE_SOURCES
    src/core/Subsystem.cpp
    src/scene/Scene.cpp
)

foreach(engine_target IN ITEMS engine engine_static)
    if(TARGET ${engine_target})
        target_sources(${engine_target} PRIVATE ${ENGINE_CORE_SOURCES} ${ENGINE_CORE_HEADERS})
        # All of these appear in public headers (SimpleMath in Transform, EnTT in
        # Scene/Engine, enkiTS via Engine::GetTaskScheduler()), so consumers need them too.
        target_link_libraries(${engine_target} PUBLIC Microsoft::DirectXTK EnTT::EnTT enkiTS::enkiTS)
    endif()
endforeach()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ENGINE_CORE_HEADERS} ${ENGINE_CORE_SOURCES})
