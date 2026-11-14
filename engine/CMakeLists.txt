# ---------------------------------------------------------------------------
# engine -- the SDK. Built as a shared library; the editor links against it.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(RmlUi 6.3 CONFIG REQUIRED)

set(ENGINE_UI_SOURCES
    src/ui/UiDx11Renderer.cpp
    src/ui/UiInterfaces.cpp
    src/ui/UiModelStorage.cpp
    src/ui/UiSubsystem.cpp
)
set(ENGINE_UI_HEADERS
    include/engine/ui/UiHost.hpp
    include/engine/ui/UiModel.hpp
    include/engine/ui/UiSubsystem.hpp
    include/engine/ui/UiTypes.hpp
    include/engine/input/InputUiEvent.hpp
)

# Keep upstream backend warnings separate from the engine's warning policy.
set(RMLUI_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/rmlui-backend")
add_library(rmlui_dx11_backend STATIC
    "${RMLUI_BACKEND_DIR}/RmlUi_Renderer_DX11.cpp"
    "${RMLUI_BACKEND_DIR}/RmlUi_Renderer_DX11.h"
    "${RMLUI_BACKEND_DIR}/RmlUi_Include_Windows.h"
)
target_link_libraries(rmlui_dx11_backend PRIVATE RmlUi::Core d3d11 d3dcompiler dxgi)
target_compile_options(rmlui_dx11_backend PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/w>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
)
set_target_properties(rmlui_dx11_backend PROPERTIES FOLDER "external")

# --- Lua scripting ----------------------------------------------------------

find_package(Lua 5.4 EXACT REQUIRED)
find_package(sol2 CONFIG REQUIRED)

if(NOT TARGET Lua::Lua)
    add_library(Lua::Lua INTERFACE IMPORTED)

    set_target_properties(Lua::Lua PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${LUA_LIBRARIES}"
    )
endif()

set(ENGINE_PUBLIC_HEADERS
    include/engine/core/HandleMap.hpp
    include/engine/core/Log.hpp
    include/engine/core/ResourceID.hpp
    include/engine/core/Uuid.hpp
    include/engine/core/Utf.hpp
    include/engine/engine.hpp
    include/engine/rendering/DebugDraw.hpp
    include/engine/rendering/FPSCamera.hpp
    include/engine/rendering/GraphicalState.hpp
    include/engine/rendering/MaterialDrawList.hpp
    include/engine/rendering/passes/GBufferPass.hpp
    include/engine/rendering/passes/LightPass.hpp
    include/engine/rendering/passes/PresentPass.hpp
    include/engine/rendering/passes/RenderPass.hpp
    include/engine/rendering/passes/SelectionMaskPass.hpp
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
    include/engine/scripting/LuaRuntime.hpp
    include/engine/scripting/ScriptComponent.hpp
    include/engine/scripting/ScriptProperties.hpp
    include/engine/scripting/AutoloadDefinition.hpp
    include/engine/scripting/ScriptSubsystem.hpp
    include/engine/scripting/LuaBindings.hpp
    include/engine/scripting/LuaBindingsContext.hpp
    include/engine/scripting/LuaEntityHandle.hpp
    include/engine/scripting/LuaSceneBindings.hpp
    include/engine/scripting/LuaComponentBindings.hpp
    include/engine/scripting/LuaGameplayBindings.hpp
    include/engine/scripting/LuaSaveGameBindings.hpp
    include/engine/scripting/LuaStateValue.hpp
)

set(ENGINE_SOURCES
    src/scripting/ScriptProperties.cpp
    src/core/HandleMap.cpp
    src/core/Log.cpp
    src/core/ResourceID.cpp
    src/core/Uuid.cpp
    src/core/Utf.cpp
    src/engine.cpp
    src/rendering/DebugDraw.cpp
    src/rendering/FPSCamera.cpp
    src/rendering/GraphicalState.cpp
    src/rendering/passes/GBufferPass.cpp
    src/rendering/passes/LightPass.cpp
    src/rendering/passes/PresentPass.cpp
    src/rendering/passes/RenderPass.cpp
    src/rendering/passes/SelectionMaskPass.cpp
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
    src/scripting/LuaRuntime.cpp
    src/scripting/AutoloadDefinition.cpp
    src/scripting/ScriptSubsystem.cpp
    src/scripting/LuaBindings.cpp
    src/scripting/LuaEntityHandle.cpp
    src/scripting/LuaSceneBindings.cpp
    src/scripting/LuaComponentBindings.cpp
    src/scripting/LuaGameplayBindings.cpp
    src/scripting/LuaSaveGameBindings.cpp
)

set(ENGINE_SHADERS
    shaders/Common.hlsl
    shaders/DebugDraw.hlsl
    shaders/DirectionalLightVS.hlsl
    shaders/GBufferPass.hlsl
    shaders/GBufferVS.hlsl
    shaders/GBufferPS.hlsl
    shaders/PresentPass.hlsl
    shaders/SelectionMask.hlsl
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
    ${ENGINE_UI_SOURCES}
    ${ENGINE_UI_HEADERS}
    ${ENGINE_PUBLIC_HEADERS}
    ${ENGINE_SHADERS}
)
add_library(Engine::Engine ALIAS engine)

add_library(engine_static STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_UI_SOURCES}
    ${ENGINE_UI_HEADERS}
    ${ENGINE_PUBLIC_HEADERS}
    ${ENGINE_SHADERS}
)
add_library(Engine::EngineStatic ALIAS engine_static)

# All runtime targets share the same output directory. Copy common assets once
# before either engine variant is built, rather than racing per-application copies.
get_property(SRI_ENGINE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(SRI_ENGINE_MULTI_CONFIG)
    set(SRI_ENGINE_ASSET_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/engine")
else()
    set(SRI_ENGINE_ASSET_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine")
endif()
add_custom_target(EngineRuntimeAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${SRI_ENGINE_ASSET_DIR}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/primitives" "${SRI_ENGINE_ASSET_DIR}/primitives"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${SRI_ENGINE_ASSET_DIR}/ui"
    COMMENT "Copying engine runtime assets"
    VERBATIM
)
set_target_properties(EngineRuntimeAssets PROPERTIES FOLDER "engine")
add_dependencies(engine EngineRuntimeAssets)
add_dependencies(engine_static EngineRuntimeAssets)

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
        ${RMLUI_BACKEND_DIR}
)

target_include_directories(engine_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${RMLUI_BACKEND_DIR}
)

# The generated header makes ENGINE_API empty for static consumers.
target_compile_definitions(engine_static PUBLIC ENGINE_STATIC_DEFINE)

# PRIVATE where the type does not appear in a public header -- that way the
# editor does not have to find_package() our implementation details.
target_link_libraries(engine
    PUBLIC
        spdlog::spdlog Microsoft::DirectXTK
    PRIVATE
        d3d11 d3dcompiler dxgi dxguid ole32 imm32 windowscodecs
        rmlui_dx11_backend RmlUi::Core $<$<CONFIG:Debug>:RmlUi::Debugger>
)

target_link_libraries(engine_static
    PUBLIC
        spdlog::spdlog Microsoft::DirectXTK
    PRIVATE
        d3d11 d3dcompiler dxgi dxguid ole32 imm32 windowscodecs
        rmlui_dx11_backend RmlUi::Core $<$<CONFIG:Debug>:RmlUi::Debugger>
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
include(CMakePackageConfigHelpers)

set_target_properties(engine PROPERTIES EXPORT_NAME Engine)
if(WIN32)
    # Public headers transitively include Windows headers and use numeric_limits::max().
    target_compile_definitions(engine INTERFACE $<INSTALL_INTERFACE:NOMINMAX>)
endif()
configure_package_config_file(cmake/EngineConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/EngineConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Engine"
)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/EngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion
)

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
if(COMMAND x_vcpkg_install_local_dependencies)
    x_vcpkg_install_local_dependencies(
        TARGETS engine DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk
    )
endif()
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/EngineConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/EngineConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Engine COMPONENT sdk
)
sri_install_dependency_notices(sdk)

# --- Core kernel -------------------------------------------------------------
# Engine lifecycle, subsystems, events and the scene model -- the API
# every other system (rendering, input, audio, editor, ...) builds on.

find_package(EnTT CONFIG REQUIRED)
find_package(enkiTS CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)

# Embed the schema so material parsing does not depend on runtime file locations.
set(MATERIAL_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/material.schema.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MATERIAL_SCHEMA_FILE}")
file(READ "${MATERIAL_SCHEMA_FILE}" MATERIAL_SCHEMA_JSON)
configure_file(cmake/MaterialSchema.hpp.in generated/MaterialSchema.hpp @ONLY)

set(ENGINE_CORE_HEADERS
    include/engine/input/CursorPolicy.hpp
    include/engine/input/InputSubsystem.hpp
    include/engine/input/InputTypes.hpp
    include/engine/input/Win32InputHost.hpp
    include/engine/input/Win32PhysicalKey.hpp
    include/engine/rendering/RenderingSubsystem.hpp
    include/engine/rendering/RenderComponents.hpp
    include/engine/rendering/RenderExtractionSubsystem.hpp
    include/engine/assets/AssetCommon.hpp
    include/engine/assets/AssetAuthoring.hpp
    include/engine/assets/AssetCatalog.hpp
    include/engine/assets/AssetEvents.hpp
    include/engine/assets/AssetHandle.hpp
    include/engine/assets/AssetPathResolver.hpp
    include/engine/assets/AssetSystem.hpp
    include/engine/assets/AssetTable.hpp
    include/engine/assets/AssetTypes.hpp
    include/engine/assets/AssetPipeline.hpp
    include/engine/assets/MaterialDefinition.hpp
    include/engine/core/CoreEvents.hpp
    include/engine/core/FileSystem.hpp
    include/engine/core/FrameContext.hpp
    include/engine/core/PathUtils.hpp
    include/engine/core/Subsystem.hpp
    include/engine/core/Surface.hpp
    include/engine/project/Project.hpp
    include/engine/scene/Components.hpp
    include/engine/scene/Controller.hpp
    include/engine/scene/StateValue.hpp
    include/engine/scene/SceneQueries.hpp
    include/engine/scene/Scene.hpp
    include/engine/scene/SceneDocument.hpp
    include/engine/scene/GameDocument.hpp
    include/engine/physics/PhysicsComponents.hpp
    include/engine/physics/PhysicsSubsystem.hpp
    include/engine/scene/Transform.hpp
)

set(ENGINE_CORE_SOURCES
    src/rendering/RenderingSubsystem.cpp
    src/input/InputSubsystem.cpp
    src/input/Win32InputHost.cpp
    src/rendering/RenderExtractionSubsystem.cpp
    src/assets/AssetPathResolver.cpp
    src/assets/AssetAuthoring.cpp
    src/assets/AssetCatalog.cpp
    src/assets/AssetSystem.cpp
    src/assets/AssimpMeshImporter.cpp
    src/assets/ShaderImporter.cpp
    src/assets/TextureImporter.cpp
    src/assets/MaterialDefinition.cpp
    src/assets/MaterialImporter.cpp
    src/core/FileSystem.cpp
    src/core/Subsystem.cpp
    src/physics/PhysicsComponents.cpp
    src/physics/PhysicsSubsystem.cpp
    src/project/Project.cpp
    src/scene/Scene.cpp
    src/scene/SceneDocument.cpp
    src/scene/GameDocument.cpp
    src/scene/StateValue.cpp
    src/scene/SceneQueries.cpp
)

foreach(engine_target IN ITEMS engine engine_static)
    if(TARGET ${engine_target})
        target_compile_options(${engine_target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
        # Type-check Lua arguments in every config: unchecked Release bindings crash on wrong argument types. TO BE REMOVED
        target_compile_definitions(${engine_target} PUBLIC SOL_ALL_SAFETIES_ON=1)
        target_sources(${engine_target} PRIVATE ${ENGINE_CORE_SOURCES} ${ENGINE_CORE_HEADERS})
        target_include_directories(${engine_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        # All of these appear in public headers (SimpleMath in Transform, EnTT in
        # Scene/Engine, enkiTS via Engine::GetTaskScheduler()), so consumers need them too.
        target_link_libraries(${engine_target} PUBLIC Microsoft::DirectXTK EnTT::EnTT enkiTS::enkiTS)
        # Assimp is PRIVATE on purpose: AssimpMeshImporter.cpp is its only user and no
        # public header mentions it. Static consumers still get it via the link-only interface.
        target_link_libraries(${engine_target} PRIVATE assimp::assimp nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator stduuid Lua::Lua sol2::sol2 Jolt::Jolt)
    endif()
endforeach()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${ENGINE_CORE_HEADERS} ${ENGINE_CORE_SOURCES})

add_executable(sri-asset-meta tools/asset_meta.cpp)
target_link_libraries(sri-asset-meta PRIVATE Engine::EngineStatic)
set_project_warnings(sri-asset-meta)
