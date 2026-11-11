cmake_minimum_required(VERSION 3.30)

project(dbpp VERSION 0.1.0 DESCRIPTION "ADBC data manager built with HuxerUI" LANGUAGES NONE)

# Local HuxerUI checkout (farna/main branch) and extension libraries.
set(DBPP_HUXERUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../HuxerUI-fork" CACHE PATH "HuxerUI source directory")
set(DBPP_DATATABLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Lib-DataTable" CACHE PATH "Lib-DataTable source directory")
set(DBPP_ADBC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adbc" CACHE PATH "Apache Arrow ADBC source directory")

if (NOT EXISTS "${DBPP_HUXERUI_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DBPP_HUXERUI_DIR does not contain a HuxerUI checkout: ${DBPP_HUXERUI_DIR}")
endif ()
if (NOT EXISTS "${DBPP_DATATABLE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DBPP_DATATABLE_DIR does not contain a Lib-DataTable checkout: ${DBPP_DATATABLE_DIR}")
endif ()

set(HUXERUI_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_TESTING_LIBRARY OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HUXERUI_BUILD_CLI OFF CACHE BOOL "" FORCE)

add_subdirectory("${DBPP_HUXERUI_DIR}" "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)

enable_language(C)
enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()

# ---- Apache Arrow ADBC (driver manager + SQLite driver) -------------------
# ADBC is the database abstraction: every connection goes through the driver
# manager, so building against PostgreSQL/MySQL/DuckDB/etc. is a driver swap,
# not an application change.
if (NOT EXISTS "${DBPP_ADBC_DIR}/c/CMakeLists.txt")
    message(FATAL_ERROR "DBPP_ADBC_DIR does not contain an ADBC source tree: ${DBPP_ADBC_DIR}")
endif ()
set(ADBC_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(ADBC_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(ADBC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ADBC_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ADBC_BUILD_INTEGRATION OFF CACHE BOOL "" FORCE)
set(ADBC_DRIVER_MANAGER ON CACHE BOOL "" FORCE)
set(ADBC_DRIVER_SQLITE ON CACHE BOOL "" FORCE)
set(ADBC_DRIVER_POSTGRESQL OFF CACHE BOOL "" FORCE)
set(ADBC_DRIVER_FLIGHTSQL OFF CACHE BOOL "" FORCE)
set(ADBC_WITH_VENDORED_FMT ON CACHE BOOL "" FORCE)
set(ADBC_WITH_VENDORED_NANOARROW ON CACHE BOOL "" FORCE)
set(ADBC_DEFINE_COMMON_ENTRYPOINTS ON CACHE BOOL "" FORCE)
set(ADBC_USE_CCACHE OFF CACHE BOOL "" FORCE)
add_subdirectory("${DBPP_ADBC_DIR}/c" "${CMAKE_BINARY_DIR}/adbc" EXCLUDE_FROM_ALL)

file(GLOB_RECURSE DBPP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

if (WIN32)
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
elseif (APPLE)
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
endif ()

huxerui_add_app(dbpp
        SOURCES
            ${DBPP_SOURCES}
        BUNDLE_NAME
            "db++"
        BUNDLE_IDENTIFIER
            "dev.farna.dbpp"
)

huxerui_use_library(dbpp
        TARGET HuxerUI::DataTable
        PATH "${DBPP_DATATABLE_DIR}"
)

target_include_directories(dbpp PRIVATE
        "${DBPP_ADBC_DIR}/c/include"
        "${DBPP_ADBC_DIR}/c/vendor"
)
target_link_libraries(dbpp PRIVATE
        adbc_driver_manager_shared
        nanoarrow
)
target_compile_definitions(dbpp PRIVATE
        DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
)
add_dependencies(dbpp adbc_driver_sqlite_shared)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dbpp PRIVATE -Wall -Wextra -Wpedantic)
endif ()

# The platform entry point lives in platform/linux/main.cpp; install a run script
# next to the build output for convenience.
add_custom_target(dbpp-run
        COMMAND "$<TARGET_FILE:dbpp>"
        DEPENDS dbpp
        USES_TERMINAL
)
