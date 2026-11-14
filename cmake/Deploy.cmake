# ---------------------------------------------------------------------------
# Deploy.cmake -- portable, self-contained app folders.
#
#   sri_add_deploy(<app> [LIBRARIES <shared library targets>...])
#
# Installs <app>, the listed shared libraries, the vcpkg DLLs they import and
# the MSVC runtime into install component <app>. The layout is flat, like the
# build tree: the executable sits in the folder root next to its DLLs, because
# the engine resolves its files relative to the executable.
#
# Also adds a `deploy_<app>` target which builds <app> and installs its
# component into ${SRI_DEPLOY_DIR}/<config>/<app>; `deploy` runs all of them:
#   cmake --build --preset windows-release --target deploy
#
# Runtime files beyond binaries (shaders, assets, Qt plugins) are the caller's
# job: install(... COMPONENT <app>).
#
#   sri_install_docs(<component> <destination> [SDK])
#
# Installs docs/, so game developers and their AI agents can read the guides
# next to the editor or SDK. docs/dev (engine-team notes) never ships;
# docs/sdk (C++ host docs) ships only with SDK.
# ---------------------------------------------------------------------------

set(SRI_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy"
    CACHE PATH "Where the deploy_* targets put the portable app folders")

# Only collect the MSVC runtime DLL list here; sri_add_deploy() installs it per app.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)

# Preserve vcpkg's per-port notice layout, including notices for transitive
# dependencies. Font notices also travel with the installed engine/ui assets.
function(sri_install_dependency_notices component)
    install(FILES "${PROJECT_SOURCE_DIR}/external/rmlui-backend/LICENSE.txt"
        DESTINATION licenses/rmlui-backend COMPONENT ${component})
    install(FILES "${PROJECT_SOURCE_DIR}/engine/ui/fonts/LICENSE"
        DESTINATION licenses/engine-fonts COMPONENT ${component})
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET
       AND EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
        install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/"
            DESTINATION licenses/vcpkg COMPONENT ${component}
            FILES_MATCHING PATTERN "copyright" PATTERN "LICENSE*" PATTERN "license*")
    endif()
endfunction()

function(sri_install_docs component destination)
    cmake_parse_arguments(PARSE_ARGV 2 arg "SDK" "" "")
    set(exclude PATTERN dev EXCLUDE)
    if(NOT arg_SDK)
        list(APPEND exclude PATTERN sdk EXCLUDE)
    endif()
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/"
        DESTINATION ${destination} COMPONENT ${component} ${exclude})
endfunction()

function(sri_add_deploy app)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "LIBRARIES")

    install(TARGETS ${app} RUNTIME DESTINATION . COMPONENT ${app})
    # install(FILES) rather than install(TARGETS): the latter would also drag
    # the import libraries along.
    foreach(lib IN LISTS arg_LIBRARIES)
        install(FILES "$<TARGET_FILE:${lib}>" DESTINATION . COMPONENT ${app})
    endforeach()

    # Scans the installed binaries and copies the vcpkg DLLs they import
    # (recursively, from the debug or release tree to match the config).
    # Must come after the binaries above are installed.
    if(COMMAND x_vcpkg_install_local_dependencies)
        x_vcpkg_install_local_dependencies(
            TARGETS ${app} ${arg_LIBRARIES}
            DESTINATION .
            COMPONENT ${app}
        )
    else()
        message(WARNING "sri_add_deploy(${app}): not using the vcpkg toolchain, "
            "vcpkg DLLs will be missing from the deployed folder")
    endif()

    # The debug runtime is not redistributable; Debug deploys only run where
    # Visual Studio is installed anyway.
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            DESTINATION .
            COMPONENT ${app}
            CONFIGURATIONS Release RelWithDebInfo MinSizeRel
        )
    endif()
    sri_install_dependency_notices(${app})

    # Wipe first so files removed from the project do not linger in the folder.
    set(out "${SRI_DEPLOY_DIR}/$<CONFIG>/${app}")
    add_custom_target(deploy_${app}
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${out}"
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}"
                --config $<CONFIG> --component ${app} --prefix "${out}"
        COMMENT "Deploying ${app}"
        VERBATIM
    )
    add_dependencies(deploy_${app} ${app})
    set_target_properties(deploy_${app} PROPERTIES FOLDER "deploy")

    if(NOT TARGET deploy)
        add_custom_target(deploy)
        set_target_properties(deploy PROPERTIES FOLDER "deploy")
    endif()
    add_dependencies(deploy deploy_${app})
endfunction()
