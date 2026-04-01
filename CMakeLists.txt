cmake_minimum_required(VERSION 3.21)

project(AMXX_MariaDB_Module LANGUAGES C CXX)

set(MODULE_FILENAME "mariadb")
set(METAMOD_R_TAG "master" CACHE STRING "Pinned Metamod-R ref")
set(MARIADB_CONNECTOR_TAG "v3.4.8" CACHE STRING "Pinned MariaDB Connector/C tag")

if(MSVC)
    # Win32 is selected via the generator, for example: -A Win32
elseif(UNIX)
    message(STATUS "Forcing 32-bit toolchain for UNIX build.")
    set(CMAKE_C_FLAGS "-m32" CACHE STRING "C compiler flags" FORCE)
    set(CMAKE_CXX_FLAGS "-m32" CACHE STRING "C++ compiler flags" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS "-m32 -static-libgcc -static-libstdc++" CACHE STRING "Shared linker flags" FORCE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # If a custom OpenSSL include dir is provided (example: static OpenSSL built for
    # i386 compatibility), append it to the compiler flags. Using CMAKE_C_FLAGS is the
    # most reliable way to reach all targets including OBJECT libs in FetchContent
    # subdirectories, where directory-scope include_directories() does not propagate.
    if(DEFINED OPENSSL_INCLUDE_DIR AND NOT OPENSSL_INCLUDE_DIR STREQUAL "")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${OPENSSL_INCLUDE_DIR}" CACHE STRING "C compiler flags" FORCE)
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
    add_compile_options(/W3 /wd4244 /wd4800 /wd4996 /wd4530 /EHsc)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unused-parameter)
endif()

include(FetchContent)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_Declare(
    metamod_r
    GIT_REPOSITORY https://github.com/rehlds/Metamod-R.git
    GIT_TAG ${METAMOD_R_TAG}
)
FetchContent_GetProperties(metamod_r)
if(NOT metamod_r_POPULATED)
    # Metamod-R provides the headers we need, but its own CMake project does not
    # support Windows. Populate the source tree without adding it as a subproject.
    FetchContent_Populate(metamod_r)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_MYSQLCOMPAT OFF CACHE BOOL "" FORCE)
set(WITH_CURL OFF CACHE BOOL "" FORCE)
set(WITH_EXTERNAL_ZLIB OFF CACHE BOOL "" FORCE)
set(DEFAULT_SSL_VERIFY_SERVER_CERT OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(WITH_SSL SCHANNEL CACHE STRING "" FORCE)
endif()
set(REMOTEIO_PLUGIN_TYPE OFF CACHE STRING "" FORCE)
set(AUTH_GSSAPI_PLUGIN_TYPE OFF CACHE STRING "" FORCE)
set(INSTALL_PLUGINDIR "lib/mariadb/plugin" CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_DIALOG STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CLIENT_ED25519 STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CACHING_SHA2_PASSWORD OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_SHA256_PASSWORD OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PARSEC OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_AUTH_GSSAPI_CLIENT STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PVIO_SHMEM STATIC CACHE STRING "" FORCE)

FetchContent_Declare(
    mariadb_connector_c
    GIT_REPOSITORY https://github.com/mariadb-corporation/mariadb-connector-c.git
    GIT_TAG ${MARIADB_CONNECTOR_TAG}
)
FetchContent_MakeAvailable(mariadb_connector_c)

set(MARIADB_CLIENT_TARGET "")
if(TARGET mariadbclient)
    set(MARIADB_CLIENT_TARGET mariadbclient)
elseif(TARGET libmariadb)
    set(MARIADB_CLIENT_TARGET libmariadb)
endif()

if(NOT MARIADB_CLIENT_TARGET)
    message(FATAL_ERROR "Could not find the MariaDB Connector/C static library target.")
endif()

add_library(amxx_sdk STATIC
    src/sdk/amxmodx/amxxmodule.cpp
)

target_include_directories(amxx_sdk PUBLIC
    src
    src/sdk/amxmodx
    "${metamod_r_SOURCE_DIR}/metamod/src"
    "${metamod_r_SOURCE_DIR}/metamod/include/common"
    "${metamod_r_SOURCE_DIR}/metamod/include/dlls"
    "${metamod_r_SOURCE_DIR}/metamod/include/engine"
    "${metamod_r_SOURCE_DIR}/metamod/include/game_shared"
    "${metamod_r_SOURCE_DIR}/metamod/include/pm_shared"
    "${metamod_r_SOURCE_DIR}/metamod/include/public"
)

target_compile_definitions(amxx_sdk PUBLIC
    AMX_NO_MALLOC
    HAVE_STDINT_H
    NO_MSVC8_AUTO_COMPAT
    $<$<PLATFORM_ID:Windows>:WIN32;_CRT_SECURE_NO_DEPRECATE>
    $<$<NOT:$<PLATFORM_ID:Windows>>:__linux__>
    # force AMXAPI to empty on Linux (Metamod-R headers can redefine it before
    # amxxmodule.h gets a chance to set the correct default)
    $<$<NOT:$<PLATFORM_ID:Windows>>:AMXAPI=>
    # tier0/dbg.h in Metamod-R uses the Windows-only _vsnprintf name
    $<$<NOT:$<PLATFORM_ID:Windows>>:_vsnprintf=vsnprintf>
)

set(MODULE_SOURCES
    src/mariadb_async.cpp
    src/mariadb_connection.cpp
    src/mariadb_module.cpp
    src/mariadb_natives.cpp
    src/mariadb_result.cpp
    src/mariadb_statement.cpp
)

if(WIN32)
    list(APPEND MODULE_SOURCES src/version.rc)
endif()

add_library(${MODULE_FILENAME}_amxx SHARED ${MODULE_SOURCES})

target_link_libraries(${MODULE_FILENAME}_amxx PRIVATE
    amxx_sdk
    ${MARIADB_CLIENT_TARGET}
)

if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    # Metamod-R looks up the undecorated symbol name. MSVC stdcall emits
    # _GiveFnptrsToDll@8 by default, so export an alias with the legacy name.
    target_link_options(${MODULE_FILENAME}_amxx PRIVATE "/EXPORT:GiveFnptrsToDll=_GiveFnptrsToDll@8")
endif()

target_include_directories(${MODULE_FILENAME}_amxx PRIVATE
    src
    src/sdk/amxmodx
    "${mariadb_connector_c_SOURCE_DIR}/include"
    "${mariadb_connector_c_BINARY_DIR}/include"
)

set_target_properties(${MODULE_FILENAME}_amxx PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/amxmodx/modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/amxmodx/modules"
)
if(WIN32)
    set_target_properties(${MODULE_FILENAME}_amxx PROPERTIES SUFFIX ".dll")
else()
    set_target_properties(${MODULE_FILENAME}_amxx PROPERTIES SUFFIX "_i386.so")
endif()
