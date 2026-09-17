# Included from the project's root CMakeLists.txt. Every path here resolves
# against this file's own directory rather than the caller's, so a submodule
# directory holding the wrong tree can never be configured as this project.
include_guard(GLOBAL)

option(UV_LIBRARY "use installed libuv instead of building from source")
option(UVW_LIBRARY "use installed uvw instead of building from source")
option(UV_TERMUX_PATCH "apply libuv_termux.diff" ${OS_ANDROID})

set(ZLL_EXTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(ZLL_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
find_package(Git QUIET)
if(NOT GIT_EXECUTABLE)
    set(GIT_EXECUTABLE git)
endif()

# A submodule counts as present only when its own marker file is there. A
# checkout that exists but contains something else is reported by name instead
# of being added as a subdirectory: a copy of this project in external/uvw
# would otherwise make CMake configure GRID0 Relay a second time and fail with
# dozens of confusing duplicate-target errors.
function(zll_require_submodule path marker name)
    set(directory "${ZLL_EXTERNAL_DIR}/${path}")
    if(NOT EXISTS "${directory}/${marker}")
        message(STATUS "Fetching ${name} via submodule")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" submodule update --init --recursive -- "external/${path}"
            WORKING_DIRECTORY "${ZLL_SOURCE_ROOT}"
            OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(EXISTS "${directory}/${marker}")
        return()
    endif()
    if(EXISTS "${directory}/CMakeLists.txt")
        message(FATAL_ERROR
            "external/${path} exists but does not contain ${name}: ${marker} is missing.\n"
            "Something else is checked out there. Delete external/${path}, then run:\n"
            "    git submodule update --init --recursive\n"
            "Building from a source archive? Download the release source package, "
            "which already includes the pinned ${name} sources.")
    endif()
    message(FATAL_ERROR
        "${name} is missing from external/${path}. Clone with submodules, or run:\n"
        "    git submodule update --init --recursive")
endfunction()

if (UV_LIBRARY)
    find_package(Libuv REQUIRED)
    add_library(uv_a STATIC IMPORTED)
    set_target_properties(uv_a PROPERTIES
        IMPORTED_LOCATION ${LIBUV_LIBRARIES}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUV_INCLUDE_DIR}
    )
else()
    zll_require_submodule(libuv "include/uv.h" "libuv")
    message(STATUS "Installing libuv via submodule")
    add_subdirectory("${ZLL_EXTERNAL_DIR}/libuv" "${CMAKE_BINARY_DIR}/external/libuv" EXCLUDE_FROM_ALL)
    target_include_directories(uv_a INTERFACE "${ZLL_EXTERNAL_DIR}/libuv/include")
    if (UV_TERMUX_PATCH)
        message(STATUS "Apply libuv_termux.diff")
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${ZLL_EXTERNAL_DIR}/patch/libuv_termux.diff"
            WORKING_DIRECTORY "${ZLL_EXTERNAL_DIR}/libuv")
    elseif(EXISTS "${ZLL_EXTERNAL_DIR}/libuv/.git")
        # Only reverse the patch when it is actually applied. Checking first
        # keeps a clean checkout from printing a failed-patch error every run.
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply -R --check "${ZLL_EXTERNAL_DIR}/patch/libuv_termux.diff"
            WORKING_DIRECTORY "${ZLL_EXTERNAL_DIR}/libuv"
            RESULT_VARIABLE termux_patch_applied OUTPUT_QUIET ERROR_QUIET)
        if(termux_patch_applied EQUAL 0)
            message(STATUS "Reverting libuv_termux.diff")
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply -R "${ZLL_EXTERNAL_DIR}/patch/libuv_termux.diff"
                WORKING_DIRECTORY "${ZLL_EXTERNAL_DIR}/libuv")
        endif()
    endif()
endif()

if (UVW_LIBRARY)
    find_package(UVW REQUIRED)
    add_library(uvw STATIC IMPORTED)
    set_target_properties(uvw PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUVW_INCLUDE_DIR}
    )
else()
    zll_require_submodule(uvw "src/uvw.hpp" "uvw")
    message(STATUS "Installing uvw via submodule")
    add_subdirectory("${ZLL_EXTERNAL_DIR}/uvw" "${CMAKE_BINARY_DIR}/external/uvw" EXCLUDE_FROM_ALL)
    include_directories("${ZLL_EXTERNAL_DIR}/uvw/src")
endif()
