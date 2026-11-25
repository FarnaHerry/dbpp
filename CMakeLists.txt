cmake_minimum_required(VERSION 3.30)

project(dbpp VERSION 0.2.3 DESCRIPTION "ADBC data manager built with HuxerUI" LANGUAGES NONE)

# HuxerUI 源码目录：优先使用 CI/本地检出的 third_party/huxerui，回落到同级 ../HuxerUI-fork。
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui/CMakeLists.txt")
    set(_dbpp_default_huxerui_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui")
else ()
    set(_dbpp_default_huxerui_dir "${CMAKE_CURRENT_SOURCE_DIR}/../HuxerUI-fork")
endif ()
set(DBPP_HUXERUI_DIR "${_dbpp_default_huxerui_dir}" CACHE PATH "HuxerUI source directory")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Lib-SQLite/CMakeLists.txt")
    set(_dbpp_default_sqlite_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Lib-SQLite")
else ()
    set(_dbpp_default_sqlite_lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/../HuxerUI-libs/Lib-SQLite")
endif ()
set(DBPP_SQLITE_LIB_DIR "${_dbpp_default_sqlite_lib_dir}" CACHE PATH "HuxerUI Lib-SQLite source directory")
set(DBPP_DATATABLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Lib-DataTable" CACHE PATH "Lib-DataTable source directory")
set(DBPP_ADBC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adbc" CACHE PATH "Apache Arrow ADBC source directory")

option(DBPP_USE_VENDORED_SQLITE3 "Build SQLite3 from Lib-SQLite bundled sqlite source" ON)
option(DBPP_BUILD_TESTS "Build db++ headless unit tests" ON)
option(HUXERUI_PACKAGE "Configure desktop packaging and installer targets" OFF)

if (NOT EXISTS "${DBPP_HUXERUI_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DBPP_HUXERUI_DIR does not contain a HuxerUI checkout: ${DBPP_HUXERUI_DIR}")
endif ()
if (NOT EXISTS "${DBPP_SQLITE_LIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DBPP_SQLITE_LIB_DIR does not contain a Lib-SQLite checkout: ${DBPP_SQLITE_LIB_DIR}")
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
if (WIN32)
    enable_language(RC)
endif ()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
set(ADBC_INSTALL_NAME_RPATH ON CACHE BOOL "" FORCE)
add_subdirectory("${DBPP_ADBC_DIR}/c" "${CMAKE_BINARY_DIR}/adbc" EXCLUDE_FROM_ALL)

# ADBC 25 generates driver/sqlite/config.h under ${CMAKE_BINARY_DIR}/adbc but
# adds top-level ${CMAKE_BINARY_DIR} to sqlite target include paths.
foreach (_adbc_sqlite_target IN ITEMS adbc_driver_sqlite_objlib adbc_driver_sqlite_shared adbc_driver_sqlite_static)
    if (TARGET "${_adbc_sqlite_target}")
        target_include_directories("${_adbc_sqlite_target}" PRIVATE "${CMAKE_BINARY_DIR}/adbc")
    endif ()
endforeach ()

if (APPLE)
    foreach (_adbc_macos_lib IN ITEMS adbc_driver_manager_shared adbc_driver_sqlite_shared)
        if (TARGET "${_adbc_macos_lib}")
            set_target_properties("${_adbc_macos_lib}" PROPERTIES
                    MACOSX_RPATH ON
                    INSTALL_NAME_DIR "@rpath"
                    VERSION ""
                    SOVERSION ""
            )
        endif ()
    endforeach ()
endif ()

file(GLOB_RECURSE DBPP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

if (WIN32)
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake")
elseif (APPLE)
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
endif ()

huxerui_add_app(dbpp
        SOURCES
            ${DBPP_SOURCES}
        RESOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        RESOURCE_NAMESPACE
            app
        BUNDLE_NAME
            "db++"
        BUNDLE_IDENTIFIER
            "dev.farna.dbpp"
)

if (WIN32)
    if (HUXERUI_WINDOWS_MANIFEST)
        target_sources(dbpp PRIVATE "${HUXERUI_WINDOWS_MANIFEST}")
    endif ()
    if (HUXERUI_WINDOWS_RESOURCE)
        target_sources(dbpp PRIVATE "${HUXERUI_WINDOWS_RESOURCE}")
    endif ()
    target_compile_definitions(dbpp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif ()

# macOS bundle icon and Info.plist.
if (APPLE AND NOT IOS)
    set_target_properties(dbpp PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in"
    )
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/AppIcon.icns"
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(dbpp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/AppIcon.icns")
endif ()

huxerui_use_library(dbpp
        TARGET HuxerUI::DataTable
        PATH "${DBPP_DATATABLE_DIR}"
)
huxerui_use_library(dbpp
        TARGET HuxerUI::SQLite
        PATH "${DBPP_SQLITE_LIB_DIR}"
)

target_include_directories(dbpp PRIVATE
        "${DBPP_ADBC_DIR}/c/include"
)
target_include_directories(dbpp SYSTEM PRIVATE
        "${DBPP_ADBC_DIR}/c/vendor"
)
target_link_libraries(dbpp PRIVATE
        SQLite::SQLite3
        adbc_driver_manager_shared
        nanoarrow
)
if (WIN32)
    target_link_libraries(dbpp PRIVATE ws2_32)
endif ()
target_compile_definitions(dbpp PRIVATE
        DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
        DBPP_VERSION="${PROJECT_VERSION}"
)
huxerui_add_runtime_dependencies(dbpp TARGETS adbc_driver_sqlite_shared)

if (WIN32)
    add_custom_command(TARGET dbpp POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:HuxerUI::huxerui>"
                    "$<TARGET_FILE_DIR:dbpp>/$<TARGET_FILE_NAME:HuxerUI::huxerui>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:adbc_driver_manager_shared>"
                    "$<TARGET_FILE_DIR:dbpp>/$<TARGET_FILE_NAME:adbc_driver_manager_shared>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:adbc_driver_sqlite_shared>"
                    "$<TARGET_FILE_DIR:dbpp>/$<TARGET_FILE_NAME:adbc_driver_sqlite_shared>"
            COMMENT "Copying HuxerUI and ADBC runtime DLLs next to dbpp.exe"
            VERBATIM
    )
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dbpp PRIVATE -Wall -Wextra -Wpedantic)
endif ()

# ---- 平台安装与打包 -------------------------------------------------------
get_target_property(DBPP_APP_INSTALL_COMPONENT dbpp HUXERUI_APPLICATION_INSTALL_COMPONENT)
if (WIN32 AND COMMAND huxerui_configure_windows_project_package)
    if (DBPP_APP_INSTALL_COMPONENT AND NOT DBPP_APP_INSTALL_COMPONENT MATCHES "-NOTFOUND$")
        huxerui_configure_windows_project_package(dbpp "${DBPP_APP_INSTALL_COMPONENT}")
    endif ()
elseif (APPLE AND NOT IOS AND COMMAND _huxerui_install_runtime_dependencies)
    install(TARGETS dbpp
            BUNDLE DESTINATION .
            COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
    )
    _huxerui_install_runtime_dependencies(dbpp "${DBPP_APP_INSTALL_COMPONENT}"
            "$<TARGET_FILE_NAME:dbpp>.app/Contents" "MacOS/$<TARGET_FILE_NAME:dbpp>"
    )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COMMAND _huxerui_install_runtime_dependencies)
    get_target_property(DBPP_LINUX_RESOURCE_PACKAGE dbpp HUXERUI_RESOURCE_PACKAGE)
    install(TARGETS dbpp
            RUNTIME DESTINATION .
            COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
    )
    _huxerui_install_runtime_dependencies(dbpp "${DBPP_APP_INSTALL_COMPONENT}"
            . "$<TARGET_FILE_NAME:dbpp>"
    )
    if (DBPP_LINUX_RESOURCE_PACKAGE AND NOT DBPP_LINUX_RESOURCE_PACKAGE MATCHES "-NOTFOUND$")
        install(DIRECTORY "${DBPP_LINUX_RESOURCE_PACKAGE}/"
                DESTINATION "dbpp.resources"
                COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
        )
    endif ()
    install(CODE [[
file(MAKE_DIRECTORY "$ENV{DESTDIR}/usr/bin")
file(CREATE_LINK "/opt/dbpp/dbpp" "$ENV{DESTDIR}/usr/bin/dbpp" SYMBOLIC)
]]
            COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/dbpp.desktop"
            DESTINATION /usr/share/applications
            COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
            DESTINATION /usr/share/icons/hicolor/256x256/apps
            RENAME dbpp.png
            COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
    )

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" DBPP_LINUX_PROCESSOR)
    if (DBPP_LINUX_PROCESSOR MATCHES "^(x86_64|amd64|x64)$")
        set(DBPP_LINUX_RPM_ARCH "x86_64")
        set(DBPP_LINUX_DEB_ARCH "amd64")
    elseif (DBPP_LINUX_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(DBPP_LINUX_RPM_ARCH "aarch64")
        set(DBPP_LINUX_DEB_ARCH "arm64")
    else ()
        message(FATAL_ERROR "Linux packages are unsupported for CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
    endif ()

    set(CPACK_PACKAGE_NAME "dbpp")
    set(CPACK_PACKAGE_VENDOR "FarnaHerry")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SQLite-first desktop database manager powered by Apache Arrow ADBC")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_CONTACT "FarnaHerry")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/dbpp")
    set(CPACK_GENERATOR "DEB;RPM")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "FarnaHerry")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${DBPP_LINUX_DEB_ARCH}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Databases")
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "${DBPP_LINUX_RPM_ARCH}")
    set(CPACK_COMPONENTS_ALL "${DBPP_APP_INSTALL_COMPONENT}")
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};dbpp;${DBPP_APP_INSTALL_COMPONENT};/")
    include(CPack)
endif ()

# ---- 无窗口核心单元测试 ---------------------------------------------------
if (DBPP_BUILD_TESTS)
    enable_testing()
    add_executable(test_adbc_core
            tests/test_adbc_core.cpp
            src/adbc_session.cpp
            src/redis_client.cpp
            src/driver_installer.cpp
    )
    target_compile_features(test_adbc_core PRIVATE cxx_std_20)
    target_include_directories(test_adbc_core PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${DBPP_ADBC_DIR}/c/include"
    )
    target_include_directories(test_adbc_core SYSTEM PRIVATE
            "${DBPP_ADBC_DIR}/c/vendor"
    )
    target_link_libraries(test_adbc_core PRIVATE
            HuxerUI::huxerui
            HuxerUI::DataTable
            adbc_driver_manager_shared
            nanoarrow
    )
    if (WIN32)
        target_link_libraries(test_adbc_core PRIVATE ws2_32)
    endif ()
    target_compile_definitions(test_adbc_core PRIVATE
            DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
    )
    add_dependencies(test_adbc_core adbc_driver_sqlite_shared)
    if (MSVC)
        target_compile_options(test_adbc_core PRIVATE /utf-8)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_adbc_core PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
    if (WIN32)
        target_compile_definitions(test_adbc_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        add_custom_command(TARGET test_adbc_core POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:HuxerUI::huxerui>"
                        "$<TARGET_FILE_DIR:test_adbc_core>/$<TARGET_FILE_NAME:HuxerUI::huxerui>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_manager_shared>"
                        "$<TARGET_FILE_DIR:test_adbc_core>/$<TARGET_FILE_NAME:adbc_driver_manager_shared>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_sqlite_shared>"
                        "$<TARGET_FILE_DIR:test_adbc_core>/$<TARGET_FILE_NAME:adbc_driver_sqlite_shared>"
                COMMENT "Copying runtime DLLs next to test_adbc_core.exe"
                VERBATIM
        )
    endif ()
    add_test(NAME adbc_core COMMAND test_adbc_core)

    add_executable(test_cli
            tests/test_cli.cpp
            src/cli.cpp
            src/config_db.cpp
            src/db.cpp
            src/adbc_session.cpp
            src/redis_client.cpp
            src/driver_installer.cpp
            src/settings.cpp
            src/theme_palette.cpp
    )
    target_compile_features(test_cli PRIVATE cxx_std_20)
    target_include_directories(test_cli PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${DBPP_ADBC_DIR}/c/include"
    )
    target_include_directories(test_cli SYSTEM PRIVATE
            "${DBPP_ADBC_DIR}/c/vendor"
    )
    target_link_libraries(test_cli PRIVATE
            HuxerUI::huxerui
            HuxerUI::DataTable
            HuxerUI::SQLite
            SQLite::SQLite3
            adbc_driver_manager_shared
            nanoarrow
    )
    if (WIN32)
        target_link_libraries(test_cli PRIVATE ws2_32)
    endif ()
    target_compile_definitions(test_cli PRIVATE
            DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
            DBPP_VERSION="${PROJECT_VERSION}"
    )
    add_dependencies(test_cli adbc_driver_sqlite_shared)
    if (MSVC)
        target_compile_options(test_cli PRIVATE /utf-8)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_cli PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
    if (WIN32)
        target_compile_definitions(test_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        add_custom_command(TARGET test_cli POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:HuxerUI::huxerui>"
                        "$<TARGET_FILE_DIR:test_cli>/$<TARGET_FILE_NAME:HuxerUI::huxerui>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_manager_shared>"
                        "$<TARGET_FILE_DIR:test_cli>/$<TARGET_FILE_NAME:adbc_driver_manager_shared>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_sqlite_shared>"
                        "$<TARGET_FILE_DIR:test_cli>/$<TARGET_FILE_NAME:adbc_driver_sqlite_shared>"
                COMMENT "Copying runtime DLLs next to test_cli.exe"
                VERBATIM
        )
    endif ()
    add_test(NAME cli COMMAND test_cli)

    add_executable(test_instance
            tests/test_instance.cpp
            src/instance.cpp
            src/adbc_session.cpp
            src/redis_client.cpp
    )
    target_compile_features(test_instance PRIVATE cxx_std_20)
    target_include_directories(test_instance PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${DBPP_ADBC_DIR}/c/include"
    )
    target_include_directories(test_instance SYSTEM PRIVATE
            "${DBPP_ADBC_DIR}/c/vendor"
    )
    target_link_libraries(test_instance PRIVATE
            HuxerUI::huxerui
            HuxerUI::DataTable
            adbc_driver_manager_shared
            nanoarrow
    )
    if (WIN32)
        target_link_libraries(test_instance PRIVATE ws2_32)
    endif ()
    target_compile_definitions(test_instance PRIVATE
            DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
    )
    add_dependencies(test_instance adbc_driver_sqlite_shared)
    if (MSVC)
        target_compile_options(test_instance PRIVATE /utf-8)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_instance PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
    if (WIN32)
        target_compile_definitions(test_instance PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        add_custom_command(TARGET test_instance POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:HuxerUI::huxerui>"
                        "$<TARGET_FILE_DIR:test_instance>/$<TARGET_FILE_NAME:HuxerUI::huxerui>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_manager_shared>"
                        "$<TARGET_FILE_DIR:test_instance>/$<TARGET_FILE_NAME:adbc_driver_manager_shared>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_sqlite_shared>"
                        "$<TARGET_FILE_DIR:test_instance>/$<TARGET_FILE_NAME:adbc_driver_sqlite_shared>"
                COMMENT "Copying runtime DLLs next to test_instance.exe"
                VERBATIM
        )
    endif ()
    add_test(NAME instance COMMAND test_instance)

    add_executable(test_redis
            tests/test_redis.cpp
            src/redis_client.cpp
            src/adbc_session.cpp
            src/db.cpp
            src/cli.cpp
            src/driver_installer.cpp
            src/settings.cpp
            src/theme_palette.cpp
    )
    target_compile_features(test_redis PRIVATE cxx_std_20)
    target_include_directories(test_redis PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${DBPP_ADBC_DIR}/c/include"
    )
    target_include_directories(test_redis SYSTEM PRIVATE
            "${DBPP_ADBC_DIR}/c/vendor"
    )
    target_link_libraries(test_redis PRIVATE
            HuxerUI::huxerui
            HuxerUI::DataTable
            SQLite::SQLite3
            adbc_driver_manager_shared
            nanoarrow
    )
    target_compile_definitions(test_redis PRIVATE
            DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
            DBPP_VERSION="${PROJECT_VERSION}"
    )
    add_dependencies(test_redis adbc_driver_sqlite_shared)
    if (MSVC)
        target_compile_options(test_redis PRIVATE /utf-8)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_redis PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
    if (WIN32)
        target_link_libraries(test_redis PRIVATE ws2_32)
        target_compile_definitions(test_redis PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        add_custom_command(TARGET test_redis POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:HuxerUI::huxerui>"
                        "$<TARGET_FILE_DIR:test_redis>/$<TARGET_FILE_NAME:HuxerUI::huxerui>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_manager_shared>"
                        "$<TARGET_FILE_DIR:test_redis>/$<TARGET_FILE_NAME:adbc_driver_manager_shared>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:adbc_driver_sqlite_shared>"
                        "$<TARGET_FILE_DIR:test_redis>/$<TARGET_FILE_NAME:adbc_driver_sqlite_shared>"
                COMMENT "Copying runtime DLLs next to test_redis.exe"
                VERBATIM
        )
    endif ()
    add_test(NAME redis COMMAND test_redis)
endif ()

add_custom_target(dbpp-run
        COMMAND "$<TARGET_FILE:dbpp>"
        DEPENDS dbpp
        USES_TERMINAL
)
