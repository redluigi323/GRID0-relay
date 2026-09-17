cmake_minimum_required(VERSION 3.24)
project (Grid0Relay VERSION 0.6.10 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU)
    set(CMAKE_C_FLAGS_DEBUG
        "-g3 -ggdb -g"
    )
endif()

include(TestBigEndian)
test_big_endian(BIG_ENDIAN)
if (BIG_ENDIAN)
    add_definitions(-DLANPLAY_BIG_ENDIAN)
else ()
    add_definitions(-DLANPLAY_LITTLE_ENDIAN)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-DLANPLAY_LINUX)

    # How to detect android or termux in cmake? https://github.com/termux/termux-packages/issues/151
    EXECUTE_PROCESS(COMMAND uname -o COMMAND tr -d '\n' OUTPUT_VARIABLE OPERATING_SYSTEM)
    if(${OPERATING_SYSTEM} MATCHES "Android")
        set(OS_ANDROID 1)
    endif(${OPERATING_SYSTEM} MATCHES "Android")
elseif(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    add_definitions(-DLANPLAY_FREEBSD)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_definitions(-DLANPLAY_DARWIN)
endif()

# add external libraries
include(external/external.cmake)

set(LANPLAY_VERSION "grid0-relay-${PROJECT_VERSION}")

add_subdirectory(base)
add_subdirectory(lwip)
add_subdirectory(uv_lwip)
add_subdirectory(src)

# The AppImage's relay and launcher are run by pkexec from outside the mounted
# image, where only the host's libraries are visible. Linking the GCC runtimes
# statically keeps them working on a host whose libstdc++ is older than the
# build machine's.
option(ZLL_STATIC_RUNTIME "Link libstdc++/libgcc statically into the relay and launcher" OFF)
if(ZLL_STATIC_RUNTIME)
    target_link_options(grid0-relay PRIVATE -static-libstdc++ -static-libgcc)
endif()

option(ZLL_BUILD_GUI "Build the Qt desktop application" OFF)
if(ZLL_BUILD_GUI)
    add_subdirectory(desktop)
endif()
