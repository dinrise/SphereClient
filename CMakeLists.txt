cmake_minimum_required(VERSION 3.20)

project(SphereNewClient LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(newclient_common INTERFACE)
target_include_directories(newclient_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

set(SPHERE_LUA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.5.0/src" CACHE PATH "Lua 5.5 source directory")
if(NOT EXISTS "${SPHERE_LUA_SOURCE_DIR}/lua.h" OR NOT EXISTS "${SPHERE_LUA_SOURCE_DIR}/lauxlib.c")
    message(FATAL_ERROR "Lua 5.5 source package not found under ${SPHERE_LUA_SOURCE_DIR}")
endif()
add_library(lua55 STATIC
    third_party/lua-5.5.0/src/lapi.c
    third_party/lua-5.5.0/src/lauxlib.c
    third_party/lua-5.5.0/src/lbaselib.c
    third_party/lua-5.5.0/src/lcode.c
    third_party/lua-5.5.0/src/lcorolib.c
    third_party/lua-5.5.0/src/lctype.c
    third_party/lua-5.5.0/src/ldblib.c
    third_party/lua-5.5.0/src/ldebug.c
    third_party/lua-5.5.0/src/ldo.c
    third_party/lua-5.5.0/src/ldump.c
    third_party/lua-5.5.0/src/lfunc.c
    third_party/lua-5.5.0/src/lgc.c
    third_party/lua-5.5.0/src/linit.c
    third_party/lua-5.5.0/src/liolib.c
    third_party/lua-5.5.0/src/llex.c
    third_party/lua-5.5.0/src/lmathlib.c
    third_party/lua-5.5.0/src/lmem.c
    third_party/lua-5.5.0/src/loadlib.c
    third_party/lua-5.5.0/src/lobject.c
    third_party/lua-5.5.0/src/lopcodes.c
    third_party/lua-5.5.0/src/loslib.c
    third_party/lua-5.5.0/src/lparser.c
    third_party/lua-5.5.0/src/lstate.c
    third_party/lua-5.5.0/src/lstring.c
    third_party/lua-5.5.0/src/lstrlib.c
    third_party/lua-5.5.0/src/ltable.c
    third_party/lua-5.5.0/src/ltablib.c
    third_party/lua-5.5.0/src/ltm.c
    third_party/lua-5.5.0/src/lundump.c
    third_party/lua-5.5.0/src/lutf8lib.c
    third_party/lua-5.5.0/src/lvm.c
    third_party/lua-5.5.0/src/lzio.c
)
target_include_directories(lua55 PUBLIC "${SPHERE_LUA_SOURCE_DIR}")
if(MSVC)
    target_compile_options(lua55 PRIVATE /W0)
endif()

add_executable(lua_cli
    third_party/lua-5.5.0/src/lua.c
)
target_link_libraries(lua_cli PRIVATE lua55)
set_target_properties(lua_cli PROPERTIES OUTPUT_NAME lua)
if(MSVC)
    target_compile_options(lua_cli PRIVATE /W0)
endif()

add_executable(luac
    third_party/lua-5.5.0/src/luac.c
)
target_link_libraries(luac PRIVATE lua55)
if(MSVC)
    target_compile_options(luac PRIVATE /W0)
endif()

add_library(newclient_mbc STATIC
    src/mbc/mbc.cpp
)
target_link_libraries(newclient_mbc PUBLIC newclient_common)

add_library(newclient_pe STATIC
    src/pe/pe_imports.cpp
)
target_link_libraries(newclient_pe PUBLIC newclient_common)

add_library(newclient_model STATIC
    src/model/chr.cpp
    src/model/mdl.cpp
    src/model/skl.cpp
)
target_link_libraries(newclient_model PUBLIC newclient_common)

add_executable(mbc_dump
    src/tools/mbc_dump.cpp
)
target_link_libraries(mbc_dump PRIVATE newclient_mbc)

add_executable(pe_imports
    src/tools/pe_imports_main.cpp
)
target_link_libraries(pe_imports PRIVATE newclient_pe)

add_executable(mdl_dump
    src/tools/mdl_dump.cpp
)
target_link_libraries(mdl_dump PRIVATE newclient_model)

add_executable(chr_dump
    src/tools/chr_dump.cpp
)
target_link_libraries(chr_dump PRIVATE newclient_model)

add_executable(ui_dump
    src/client/ui_definition.cpp
    src/tools/ui_dump.cpp
)
target_link_libraries(ui_dump PRIVATE newclient_common user32)

if(WIN32)
    add_executable(sphere_client WIN32
        src/client/bitmap_font.cpp
        src/client/client_runtime.cpp
        src/client/d3d_character_scene.cpp
        src/client/d3d_game_world_scene.cpp
        src/client/dds_bitmap.cpp
        src/client/lua_runtime.cpp
        src/client/main.cpp
        src/client/network_client.cpp
        src/client/skinned_character.cpp
        src/client/ui_definition.cpp
    )
    target_link_libraries(sphere_client PRIVATE newclient_common newclient_model lua55 user32 gdi32 msimg32 shell32 ws2_32 winmm d3d9)
else()
    message(STATUS "sphere_client is Win32-only; only tools will be built on this host.")
endif()
