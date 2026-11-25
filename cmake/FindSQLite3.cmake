# FindSQLite3.cmake — 当启用 DBPP_USE_VENDORED_SQLITE3（默认 ON）时，直接从
# HuxerUI Lib-SQLite（third_party/sqlite）编译静态 SQLite3 目标并提供 SQLite::SQLite3
# / SQLite3::SQLite3 别名，与 HuxerUI::SQLite 共用同一份 SQLite3 源码，避免各平台
# 打包依赖宿主 libsqlite3 动态库或 vcpkg；关闭时回落到 CMake 自带模块。

if (DEFINED DBPP_USE_VENDORED_SQLITE3 AND NOT DBPP_USE_VENDORED_SQLITE3)
    include("${CMAKE_ROOT}/Modules/FindSQLite3.cmake")
    return()
endif ()

set(_dbpp_sqlite3_candidates)
if (DEFINED DBPP_SQLITE_LIB_DIR AND NOT DBPP_SQLITE_LIB_DIR STREQUAL "")
    list(APPEND _dbpp_sqlite3_candidates "${DBPP_SQLITE_LIB_DIR}/third_party/sqlite")
endif ()
list(APPEND _dbpp_sqlite3_candidates
        "${CMAKE_SOURCE_DIR}/third_party/Lib-SQLite/third_party/sqlite"
        "${CMAKE_SOURCE_DIR}/../HuxerUI-libs/Lib-SQLite/third_party/sqlite"
)

set(_dbpp_sqlite3_dir "")
foreach (_candidate IN LISTS _dbpp_sqlite3_candidates)
    if (EXISTS "${_candidate}/sqlite3.c" AND EXISTS "${_candidate}/sqlite3.h")
        set(_dbpp_sqlite3_dir "${_candidate}")
        break()
    endif ()
endforeach ()

if (_dbpp_sqlite3_dir STREQUAL "")
    message(FATAL_ERROR "Vendored SQLite3 amalgamation not found in: ${_dbpp_sqlite3_candidates}")
endif ()

if (NOT TARGET dbpp_vendored_sqlite3)
    add_library(dbpp_vendored_sqlite3 STATIC "${_dbpp_sqlite3_dir}/sqlite3.c")
    set_target_properties(dbpp_vendored_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(dbpp_vendored_sqlite3 PUBLIC "${_dbpp_sqlite3_dir}")
    target_compile_definitions(dbpp_vendored_sqlite3 PRIVATE
            SQLITE_ENABLE_COLUMN_METADATA
            SQLITE_ENABLE_FTS5
            SQLITE_ENABLE_RTREE
            SQLITE_THREADSAFE=1
    )
    target_compile_options(dbpp_vendored_sqlite3 PRIVATE
            "$<$<C_COMPILER_ID:MSVC>:/w>"
            "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>"
    )
    if (UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(dbpp_vendored_sqlite3 PRIVATE
                Threads::Threads
                ${CMAKE_DL_LIBS}
                $<$<PLATFORM_ID:Linux>:m>
        )
    endif ()
    add_library(SQLite::SQLite3 ALIAS dbpp_vendored_sqlite3)
    add_library(SQLite3::SQLite3 ALIAS dbpp_vendored_sqlite3)
endif ()

set(SQLite3_FOUND TRUE)
set(SQLite3_INCLUDE_DIRS "${_dbpp_sqlite3_dir}")
set(SQLite3_LIBRARIES SQLite::SQLite3)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SQLite3
        REQUIRED_VARS SQLite3_INCLUDE_DIRS SQLite3_LIBRARIES
)
