# ---------------------------------------------------------------------------
# engine -- the SDK. Built as a shared library; the editor links against it.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)

set(ENGINE_PUBLIC_HEADERS
    include/engine/core/HandleMap.hpp
    include/engine/core/ResourceID.hpp
    include/engine/engine.hpp
    include/engine/rendering/FPSCamera.hpp
    include/engine/rendering/GraphicalState.hpp
    include/engine/rendering/MaterialDrawList.hpp
    include/engine/rendering/passes/GBufferPass.hpp
    include/engine/rendering/passes/LightPass.hpp
    include/engine/rendering/passes/PresentPass.hpp
    include/engine/rendering/passes/RenderPass.hpp
    include/engine/rendering/passes/ShadowPass.hpp
    include/engine/rendering/RenderWorld.hpp
    include/engine/rendering/RenderingContext.hpp
    include/engine/rendering/RenderingResourceManager.hpp
    include/engine/rendering/RenderingResources.hpp
    include/engine/rendering/RenderingSystem.hpp
    include/engine/rendering/resources/BindSlot.hpp
    include/engine/rendering/resources/Buffer.hpp
    include/engine/rendering/resources/Material.hpp
    include/engine/rendering/resources/RenderedMesh.hpp
    include/engine/rendering/resources/ShaderContext.hpp
    include/engine/rendering/resources/Texture.hpp
    include/engine/rendering/resources/VertexLayout.hpp
    include/engine/rendering/resources/VertexLayoutCache.hpp
    include/engine/rendering/UglyUtils.hpp
    include/engine/window.hpp
)

set(ENGINE_SOURCES
    src/core/HandleMap.cpp
    src/core/ResourceID.cpp
    src/engine.cpp
    src/rendering/FPSCamera.cpp
    src/rendering/GraphicalState.cpp
    src/rendering/passes/GBufferPass.cpp
    src/rendering/passes/LightPass.cpp
    src/rendering/passes/PresentPass.cpp
    src/rendering/passes/RenderPass.cpp
    src/rendering/passes/ShadowPass.cpp
    src/rendering/RenderingContext.cpp
    src/rendering/RenderingResources.cpp
    src/rendering/RenderingResourceManager.cpp
    src/rendering/RenderingSystem.cpp
    src/rendering/RenderWorld.cpp
    src/rendering/resources/BindSlot.cpp
    src/rendering/resources/Buffer.cpp
    src/rendering/resources/RenderedMesh.cpp
    src/rendering/resources/ShaderContext.cpp
    src/rendering/resources/Texture.cpp
    src/rendering/resources/VertexLayout.cpp
    src/rendering/resources/VertexLayoutCache.cpp
    src/rendering/UglyUtils.cpp
    src/window.cpp
)

set(ENGINE_SHADERS
    shaders/Common.hlsl
    shaders/DirectionalLightVS.hlsl
    shaders/GBufferPass.hlsl
    shaders/GBufferVS.hlsl
    shaders/GBufferPS.hlsl
    shaders/PresentPass.hlsl
    shaders/LightPass.hlsl
    shaders/MaterialTemplate.hlsl
    shaders/ShadowCSMPass.hlsl
    shaders/ShadowVS.hlsl
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
        d3d11 d3dcompiler dxgi dxguid ole32
)

target_link_libraries(engine_static
    PUBLIC
        spdlog::spdlog Microsoft::DirectXTK
    PRIVATE
        d3d11 d3dcompiler dxgi dxguid ole32
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

# Component "sdk", so the portable app deploys (cmake/Deploy.cmake) leave it out.
install(TARGETS engine
    EXPORT  EngineTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk
)

install(DIRECTORY include/engine
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT sdk
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/engine/engine_export.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/engine
    COMPONENT sdk
)

install(EXPORT EngineTargets
    FILE      EngineTargets.cmake
    NAMESPACE Engine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Engine
    COMPONENT sdk
)

# --- Core kernel -------------------------------------------------------------
# Engine lifecycle, subsystems, events and the scene model -- the API
# every other system (rendering, input, audio, editor, ...) builds on.

find_package(EnTT CONFIG REQUIRED)
find_package(enkiTS CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(ENGINE_CORE_HEADERS
    include/engine/input/InputSubsystem.hpp
    include/engine/input/InputTypes.hpp
    include/engine/rendering/RenderingSubsystem.hpp
    include/engine/rendering/RenderComponents.hpp
    include/engine/rendering/SceneRenderSubsystem.hpp
    include/engine/assets/AssetCommon.hpp
    include/engine/assets/AssetEvents.hpp
    include/engine/assets/AssetHandle.hpp
    include/engine/assets/AssetPathResolver.hpp
    include/engine/assets/AssetSystem.hpp
    include/engine/assets/AssetTable.hpp
    include/engine/assets/AssetTypes.hpp
    include/engine/assets/AssetPipeline.hpp
    include/engine/core/CoreEvents.hpp
    include/engine/core/FrameContext.hpp
    include/engine/core/Subsystem.hpp
    include/engine/core/Surface.hpp
    include/engine/project/Project.hpp
    include/engine/scene/Components.hpp
    include/engine/scene/Scene.hpp
    include/engine/scene/Transform.hpp
)

set(ENGINE_CORE_SOURCES
    src/rendering/RenderingSubsystem.cpp
    src/input/InputSubsystem.cpp
    src/rendering/SceneRenderSubsystem.cpp
    src/assets/AssetPathResolver.cpp
    src/assets/AssetSystem.cpp
    src/assets/AssimpMeshImporter.cpp
    src/assets/ShaderImporter.cpp
    src/assets/TextureImporter.cpp
    src/core/Subsystem.cpp
    src/project/Project.cpp
    src/scene/Scene.cpp
)

foreach(engine_target IN ITEMS engine engine_static)
    if(TARGET ${engine_target})
        target_sources(${engine_target} PRIVATE ${ENGINE_CORE_SOURCES} ${ENGINE_CORE_HEADERS})
        # All of these appear in public headers (SimpleMath in Transform, EnTT in
        # Scene/Engine, enkiTS via Engine::GetTaskScheduler()), so consumers need them too.
        target_link_libraries(${engine_target} PUBLIC Microsoft::DirectXTK EnTT::EnTT enkiTS::enkiTS)
        # Assimp is PRIVATE on purpose: AssimpMeshImporter.cpp is its only user and no
        # public header mentions it. Static consumers still get it via the link-only interface.
        target_link_libraries(${engine_target} PRIVATE assimp::assimp nlohmann_json::nlohmann_json)
    endif()
endforeach()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ENGINE_CORE_HEADERS} ${ENGINE_CORE_SOURCES})
