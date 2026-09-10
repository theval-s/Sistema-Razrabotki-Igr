# ---------------------------------------------------------------------------
# Vcpkg.cmake -- selects a vcpkg toolchain file before project() is called.
#
# Priority:
#   1. Whatever the user passed on the command line / in a preset (respected).
#   2. external/vcpkg  (git submodule), if it has been bootstrapped.
#   3. $ENV{VCPKG_ROOT} (a system-wide clone).
#
# Include this from the top-level CMakeLists BEFORE project().
# ---------------------------------------------------------------------------

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "vcpkg: using toolchain provided by the caller: ${CMAKE_TOOLCHAIN_FILE}")
    return()
endif()

get_filename_component(_sri_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_vcpkg_submodule "${_sri_root}/external/vcpkg/scripts/buildsystems/vcpkg.cmake")

if(EXISTS "${_vcpkg_submodule}")
    set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_submodule}"
        CACHE FILEPATH "vcpkg toolchain (submodule)")
    message(STATUS "vcpkg: using submodule at external/vcpkg")
elseif(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _vcpkg_root)
    set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain (VCPKG_ROOT)")
    message(STATUS "vcpkg: using VCPKG_ROOT at ${_vcpkg_root}")
else()
    message(WARNING
        "vcpkg was not found. Either initialise the submodule:\n"
        "    git submodule update --init --depth 1 external/vcpkg\n"
        "or set the VCPKG_ROOT environment variable to an existing clone.\n"
        "Configuring will continue, but find_package() for manifest "
        "dependencies will fail.")
endif()

# Manifest mode is implied by vcpkg.json at the project root; make it explicit
# so nobody accidentally gets classic-mode packages from a global install tree.
set(VCPKG_MANIFEST_MODE ON CACHE BOOL "Use vcpkg.json manifest mode")

# Note: Qt is expected to come from aqt, not vcpkg. Do not add qtbase to
# vcpkg.json unless you also drop the aqt path from CMAKE_PREFIX_PATH -- with
# both present, the vcpkg copy wins and you get a Qt nobody intended to use.
