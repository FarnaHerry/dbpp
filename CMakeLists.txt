cmake_minimum_required(VERSION 4.4)

project(dbpp VERSION 0.4.9 DESCRIPTION "ADBC data manager built with HuxerUI" LANGUAGES NONE)

# `import std` 的 experimental 门控与工具链语言必须在任何子项目前初始化。
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CxxImportStdGate)
enable_language(C)
enable_language(CXX)
if (WIN32)
    enable_language(RC)
elseif (APPLE)
    enable_language(OBJCXX)
endif ()

if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "^Ninja")
    set(_show_includes_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/DbppShowIncludes")
    file(WRITE "${_show_includes_dir}/dbpp_include_probe.h" "\n")
    file(WRITE "${_show_includes_dir}/probe.cpp" "#include \"dbpp_include_probe.h\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /Zs probe.cpp
        WORKING_DIRECTORY "${_show_includes_dir}"
        RESULT_VARIABLE _show_includes_result OUTPUT_VARIABLE _show_includes_output
        ERROR_QUIET ENCODING NONE)
    if (_show_includes_result STREQUAL "0" AND
        _show_includes_output MATCHES "(^|\n)([^\r\n]*:[ \t]+)[A-Za-z]:[^\r\n]*dbpp_include_probe\\.h")
        if (NOT CMAKE_CL_SHOWINCLUDES_PREFIX STREQUAL CMAKE_MATCH_2)
            message(STATUS "db++: corrected the MSVC /showIncludes prefix for Ninja")
            set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
        endif ()
    endif ()
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_MODULE_STD OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()

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

# ---- Apache Arrow ADBC (driver manager + SQLite driver) -------------------
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

# ---- dbpp GUI + platform sources (领域层改由 C++23 模块库 dbpp_core 提供) ----
set(DBPP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/adbc_manager_dialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/app_shell.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/connect_dialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/connection_dialogs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/schema_dialogs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/schema_tables.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/settings_page.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/sidebar.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/sidebar_groups.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/sidebar_tree.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/sql_dialogs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/sql_page.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/table_browser_page.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/table_design_page.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/table_edit_dialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/redis_tree_view.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/redis_dialogs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/value_decoder_view.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/theme.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tray.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ui_common.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/export_helpers.cpp"
)

if (WIN32)
    list(APPEND DBPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake")
elseif (APPLE)
    list(APPEND DBPP_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/native_theme.mm")
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/huxerui.cmake")
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
    target_link_libraries(dbpp PRIVATE shell32)
    target_compile_definitions(dbpp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif ()

if (APPLE AND NOT IOS)
    set_target_properties(dbpp PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in"
    )
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/AppIcon.icns"
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(dbpp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/AppIcon.icns")
    target_link_libraries(dbpp PRIVATE "-framework AppKit")
endif ()

huxerui_use_library(dbpp
        TARGET HuxerUI::DataTable
        PATH "${DBPP_DATATABLE_DIR}"
)
huxerui_use_library(dbpp
        TARGET HuxerUI::SQLite
        PATH "${DBPP_SQLITE_LIB_DIR}"
)

# ---- dbpp_core C++23 模块静态库（供 dbpp 与无窗口单元测试复用） -----------
add_library(dbpp_core STATIC
        src/settings.cpp
        src/i18n.cpp
        src/value_decoder.cpp
        src/value_decoder_json.cpp
        src/value_decoder_inflate.cpp
        src/redis_client.cpp
        src/redis_client_wire.cpp
        src/redis_client_json.cpp
        src/redis_client_domain.cpp
        src/redis_client_tree.cpp
        src/redis_client_write.cpp
        src/adbc_session.cpp
        src/adbc_session_exec.cpp
        src/adbc_arrow.cpp
        src/adbc_export.cpp
        src/adbc_xlsx.cpp
        src/adbc_metadata.cpp
        src/adbc_catalog_extras.cpp
        src/adbc_table_identity.cpp
        src/adbc_prepared.cpp
        src/adbc_ingest.cpp
        src/adbc_ingest_formats.cpp
        src/adbc_ingest_xlsx.cpp
        src/adbc_dump.cpp
        src/adbc_sql_format.cpp
        src/adbc_time.cpp
        src/adbc_session_sql.cpp
        src/adbc_session_presets.cpp
        src/driver_installer.cpp
        src/driver_installer_registry.cpp
        src/instance.cpp
        src/db.cpp
        src/db_edit.cpp
        src/db_async.cpp
        src/db_workspace.cpp
        src/db_profiles_limit.cpp
        src/mcp_server.cpp
        src/mcp_server_rpc.cpp
        src/mcp_server_tools.cpp
        src/config_db.cpp
        src/cli.cpp
        src/cli_format.cpp
        src/cli_admin.cpp
        src/cli_data.cpp
        src/cli_adbc.cpp
        src/ui/theme_palette.cpp
)
target_sources(dbpp_core PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            src/settings.cppm
            src/i18n.cppm
            src/value_decoder.cppm
            src/types.cppm
            src/redis_client.cppm
            src/adbc_session.cppm
            src/driver_installer.cppm
            src/instance.cppm
            src/db.cppm
            src/mcp_server.cppm
            src/config_db.cppm
            src/cli.cppm
)
set_target_properties(dbpp_core PROPERTIES
        CXX_MODULE_STD ON
        CXX_SCAN_FOR_MODULES ON
)
target_compile_features(dbpp_core PUBLIC cxx_std_23)
target_include_directories(dbpp_core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${DBPP_ADBC_DIR}/c/include"
)
target_include_directories(dbpp_core SYSTEM PRIVATE
        "${DBPP_ADBC_DIR}/c/vendor"
)
target_link_libraries(dbpp_core PUBLIC
        HuxerUI::huxerui
        HuxerUI::DataTable
        HuxerUI::SQLite
        SQLite::SQLite3
        adbc_driver_manager_shared
        nanoarrow
)
if (WIN32)
    target_link_libraries(dbpp_core PUBLIC ws2_32)
    target_compile_definitions(dbpp_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif ()
target_compile_definitions(dbpp_core PRIVATE
        DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
        DBPP_VERSION="${PROJECT_VERSION}"
)
add_dependencies(dbpp_core adbc_driver_sqlite_shared)
if (MSVC)
    target_compile_options(dbpp_core PRIVATE /utf-8 /wd4737 /bigobj)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dbpp_core PRIVATE -Wall -Wextra -Wpedantic)
endif ()

set_target_properties(dbpp PROPERTIES
        CXX_MODULE_STD ON
        CXX_SCAN_FOR_MODULES ON
)
target_compile_features(dbpp PRIVATE cxx_std_23)
target_include_directories(dbpp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(dbpp PRIVATE dbpp_core)
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

if (MSVC)
    target_compile_options(dbpp PRIVATE /utf-8 /wd4737 /bigobj)
    target_link_options(dbpp PRIVATE "/WHOLEARCHIVE:@cmake_cxx_std.lib")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dbpp PRIVATE -Wall -Wextra -Wpedantic)
endif ()

# ---- 平台安装与打包 -------------------------------------------------------
get_target_property(DBPP_APP_INSTALL_COMPONENT dbpp HUXERUI_APPLICATION_INSTALL_COMPONENT)
if (WIN32 AND COMMAND huxerui_configure_windows_project_package)
    if (DBPP_APP_INSTALL_COMPONENT AND NOT DBPP_APP_INSTALL_COMPONENT MATCHES "-NOTFOUND$")
        huxerui_configure_windows_project_package(dbpp "${DBPP_APP_INSTALL_COMPONENT}")
    endif ()
elseif (APPLE AND NOT IOS AND COMMAND huxerui_configure_macos_project_package)
    if (DBPP_APP_INSTALL_COMPONENT AND NOT DBPP_APP_INSTALL_COMPONENT MATCHES "-NOTFOUND$")
        huxerui_configure_macos_project_package(dbpp "${DBPP_APP_INSTALL_COMPONENT}")
        if (HUXERUI_PACKAGE)
            set(_dbpp_macos_runtime_dirs)
            if (CMAKE_CXX_COMPILER)
                get_filename_component(_dbpp_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
                get_filename_component(_dbpp_cxx_prefix "${_dbpp_cxx_bin}" DIRECTORY)
                foreach (_subdir IN ITEMS "lib/c++" "lib/unwind")
                    if (IS_DIRECTORY "${_dbpp_cxx_prefix}/${_subdir}")
                        list(APPEND _dbpp_macos_runtime_dirs "${_dbpp_cxx_prefix}/${_subdir}")
                    endif ()
                endforeach ()
            endif ()
            if (_dbpp_macos_runtime_dirs)
                huxerui_add_runtime_dependencies(dbpp
                        SEARCH_DIRECTORIES ${_dbpp_macos_runtime_dirs}
                )
            endif ()
            get_target_property(DBPP_MACOS_RESOURCE_PACKAGE dbpp HUXERUI_RESOURCE_PACKAGE)
            if (DBPP_MACOS_RESOURCE_PACKAGE AND NOT DBPP_MACOS_RESOURCE_PACKAGE MATCHES "-NOTFOUND$")
                install(DIRECTORY "${DBPP_MACOS_RESOURCE_PACKAGE}/"
                        DESTINATION "$<TARGET_FILE_NAME:dbpp>.app/Contents/Resources/HuxerUI"
                        COMPONENT "${DBPP_APP_INSTALL_COMPONENT}"
                )
            endif ()
        endif ()
    endif ()
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
    set(CPACK_RPM_PACKAGE_AUTOREQPROV "no")
    set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none")
    set(CPACK_COMPONENTS_ALL "${DBPP_APP_INSTALL_COMPONENT}")
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};dbpp;${DBPP_APP_INSTALL_COMPONENT};/")
    include(CPack)
endif ()

# ---- 无窗口核心单元测试 ---------------------------------------------------
if (DBPP_BUILD_TESTS)
    enable_testing()

    function(dbpp_add_module_test target test_name)
        add_executable("${target}" ${ARGN})
        set_target_properties("${target}" PROPERTIES
                CXX_MODULE_STD ON
                CXX_SCAN_FOR_MODULES ON
        )
        target_compile_features("${target}" PRIVATE cxx_std_23)
        target_include_directories("${target}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries("${target}" PRIVATE dbpp_core)
        target_compile_definitions("${target}" PRIVATE
                DBPP_SQLITE_DRIVER_PATH="$<TARGET_FILE:adbc_driver_sqlite_shared>"
                DBPP_VERSION="${PROJECT_VERSION}"
        )
        if (MSVC)
            target_compile_options("${target}" PRIVATE /utf-8 /wd4737 /bigobj)
            target_link_options("${target}" PRIVATE "/WHOLEARCHIVE:@cmake_cxx_std.lib")
            target_compile_definitions("${target}" PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
            add_custom_command(TARGET "${target}" POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "$<TARGET_FILE:HuxerUI::huxerui>"
                            "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:HuxerUI::huxerui>"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "$<TARGET_FILE:adbc_driver_manager_shared>"
                            "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:adbc_driver_manager_shared>"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "$<TARGET_FILE:adbc_driver_sqlite_shared>"
                            "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:adbc_driver_sqlite_shared>"
                    COMMENT "Copying runtime DLLs next to ${target}.exe"
                    VERBATIM
            )
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options("${target}" PRIVATE -Wall -Wextra -Wpedantic)
        endif ()
        add_test(NAME "${test_name}" COMMAND "${target}")
    endfunction()

    dbpp_add_module_test(test_adbc_core adbc_core tests/test_adbc_core.cpp tests/test_adbc_core_extra.cpp tests/test_adbc_capabilities.cpp)
    dbpp_add_module_test(test_cli cli tests/test_cli.cpp tests/test_cli_commands.cpp)
    dbpp_add_module_test(test_instance instance tests/test_instance.cpp)
    dbpp_add_module_test(test_redis redis tests/test_redis.cpp tests/test_redis_mock.cpp)
    dbpp_add_module_test(test_mcp mcp tests/test_mcp.cpp)
    dbpp_add_module_test(test_decoder decoder tests/test_decoder.cpp)
    dbpp_add_module_test(test_export export tests/test_export_formats.cpp)
endif ()

add_custom_target(dbpp-run
        COMMAND "$<TARGET_FILE:dbpp>"
        DEPENDS dbpp
        USES_TERMINAL
)
