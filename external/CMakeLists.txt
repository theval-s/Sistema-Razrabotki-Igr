# ---------------------------------------------------------------------------
# Third-party code that is NOT available (or not usable) through vcpkg lives
# here as a git submodule.
#
# Rule of thumb:
#   * available in vcpkg, stable release  -> vcpkg.json
#   * needs a patch / we track a fork / header-only with no port -> submodule
#
# Adding one:
#   git submodule add --depth 1 https://github.com/ocornut/imgui external/imgui
#   git -C external/imgui checkout v1.91.5
#   git add .gitmodules external/imgui && git commit
#
# Nothing here is built unless the submodule is actually checked out, so a
# fresh clone without --recurse-submodules still configures.
# ---------------------------------------------------------------------------

# Submodules must never be compiled with our warning settings, and we do not
# want their install() rules leaking into ours -> EXCLUDE_FROM_ALL.

# --- Example 1: a submodule that ships its own CMakeLists -------------------
# if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/entt/CMakeLists.txt")
#     add_subdirectory(entt EXCLUDE_FROM_ALL)
# else()
#     message(WARNING "external/entt is missing -- run: git submodule update --init")
# endif()

# --- Example 2: a submodule with no build system of its own (Dear ImGui) ----
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp")
    add_library(imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp
    )
    add_library(ThirdParty::imgui ALIAS imgui)

    # SYSTEM so that our -Wall/-W4 does not flag their headers.
    target_include_directories(imgui SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    )
    target_compile_features(imgui PUBLIC cxx_std_20)
    set_target_properties(imgui PROPERTIES FOLDER "external")
endif()

# --- Qt Advanced Docking System (editor only) ------------------------------
# A submodule rather than vcpkg: the vcpkg port depends on qtbase and would
# build a second Qt next to the aqt one the editor uses. Shared library (LGPL).
if(SRI_BUILD_EDITOR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Qt-Advanced-Docking-System/CMakeLists.txt")
        set(BUILD_EXAMPLES OFF)
        set(BUILD_STATIC OFF)
        add_subdirectory(Qt-Advanced-Docking-System EXCLUDE_FROM_ALL)
        # ADS picks its own x64/bin and x64/lib; put it next to editor.exe like Engine.dll,
        # so a rebuilt ADS is picked up even when the editor itself doesn't relink.
        set_target_properties(qtadvanceddocking-qt6 PROPERTIES
            FOLDER "external"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
        )
    else()
        message(FATAL_ERROR "external/Qt-Advanced-Docking-System is missing -- run: git submodule update --init")
    endif()
endif()

# --- ozz-animation ----------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ozz-animation/CMakeLists.txt")
    block()
        set(BUILD_SHARED_LIBS OFF)
        set(ozz_build_msvc_rt_dll ON)
        set(ozz_build_postfix OFF)
        set(ozz_build_samples OFF)
        set(ozz_build_tests OFF)
        set(ozz_build_howtos OFF)
        set(ozz_build_tools OFF)
        set(ozz_build_fbx OFF)
        set(ozz_build_gltf OFF)
        set(ozz_build_data OFF)
        add_subdirectory(ozz-animation EXCLUDE_FROM_ALL)
    endblock()
    set_target_properties(ozz_base ozz_animation ozz_animation_offline PROPERTIES
        FOLDER "external"
    )
else()
    message(FATAL_ERROR "external/ozz-animation is missing -- run: git submodule update --init")
endif()

# vcpkg itself is usually a submodule too (external/vcpkg); it is consumed by
# cmake/Vcpkg.cmake before project() and must NOT be add_subdirectory()'d here.
