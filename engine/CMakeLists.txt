# ---------------------------------------------------------------------------
# engine -- the SDK. Built as a shared library; the editor links against it.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)

set(ENGINE_PUBLIC_HEADERS
    include/engine/engine.hpp
)

set(ENGINE_SOURCES
    src/engine.cpp
)

add_library(engine SHARED
    ${ENGINE_SOURCES}
    ${ENGINE_PUBLIC_HEADERS}
)
add_library(Engine::Engine ALIAS engine)

add_library(engine_static STATIC
    ${ENGINE_SOURCES}
    ${ENGINE_PUBLIC_HEADERS}
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
    PRIVATE
        spdlog::spdlog Microsoft::DirectXTK d3d11 dxgi
)

target_link_libraries(engine_static
    PRIVATE
        spdlog::spdlog Microsoft::DirectXTK d3d11 dxgi
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
