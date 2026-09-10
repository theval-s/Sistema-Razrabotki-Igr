# ---------------------------------------------------------------------------
# set_project_warnings(<target>)
# Mostly taken from template
# -----------------------------------

option(SRI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(set_project_warnings target)
    set(msvc_warnings
        /W4
        /permissive-      # conforming mode; C++20 needs it anyway
        /Zc:__cplusplus   # otherwise __cplusplus reports 199711L
        /Zc:preprocessor  # conforming preprocessor
        /utf-8
        /w14242 /w14254 /w14263 /w14265 /w14287 /we4289
        /w14296 /w14311 /w14545 /w14546 /w14547 /w14549
        /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928
        /wd4251           # 'X' needs dll-interface: see engine/include/engine/engine.hpp
    )

    set(gcc_clang_warnings
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )

    if(MSVC)
        set(warnings ${msvc_warnings})
        if(SRI_WARNINGS_AS_ERRORS)
            list(APPEND warnings /WX)
        endif()
    else()
        set(warnings ${gcc_clang_warnings})
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND warnings -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast)
        endif()
        if(SRI_WARNINGS_AS_ERRORS)
            list(APPEND warnings -Werror)
        endif()
    endif()

    target_compile_options(${target} PRIVATE ${warnings})

    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE
        )
    endif()
endfunction()
