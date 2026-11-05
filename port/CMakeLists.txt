cmake_minimum_required(VERSION 3.20)

# ============================================================================
# hpps - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(hpps C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# HPPS_RELEASE: the build players get (tools/package_release.sh). Hand-written code (runtime libraries, HLE, renderer,
# launcher) gets buffer-overflow checks: _FORTIFY_SOURCE bounds checks on the C library's string/memory calls and
# stack canaries (-fstack-protector-strong). The generated code is left without them below: its loads and stores go
# to guest memory through XBOX_PTR, which neither check can see, and it is the hot path. ASLR, high-entropy VA and
# NX/DEP are on in every build (binutils defaults). SSE4.1 everywhere (the renderer already required it).
option(HPPS_RELEASE "Release build for players: hardening, no debug keys" OFF)
# HPPS_FORTIFY: the release's _FORTIFY_SOURCE (bounds-checked C library calls, through the headers). A player's
# main-menu keyboard hitch on Windows went away in a development build, which has none of the release's hardening
# (2026-10-25): OFF builds the release without it, the stack canaries kept, to tell which part it is
option(HPPS_FORTIFY "With HPPS_RELEASE: _FORTIFY_SOURCE=2" ON)
if(HPPS_RELEASE AND WIN32)
    add_compile_options(-msse4.1 -fstack-protector-strong)
    if(HPPS_FORTIFY)
        add_compile_options(-D_FORTIFY_SOURCE=2)
    endif()
    add_compile_definitions(HPPS_RELEASE)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # DirectSound on a host mixer (XAudio2) instead of the emulated APU.
    src/hle_dsound.c
    # The vblank count the XMV player and loading loop read (HLE device has no ISR).
    src/hle_video.c
    # XInput straight from the PC pad (SCDA style), rumble on the host.
    src/hle_input.c
    # XAPI's disc security-sector check (the synthetic DVD has none).
    src/hle_xapi.c
    # Debug: file object open/close checks ([FILEOBJ]).
    src/hle_debug.c
    # The animation start: door opening speed (RECOMP_DOOR_SPEED), [ANIM] trace (RECOMP_ANIM_TRACE).
    src/hle_anim.c
    # The debug trainer's game side (hpps_trainer.exe): noclip, spells on the buttons, health, teleport.
    src/hle_trainer.c
    # "Quit To Desktop" on the main menu (RECOMP_QUIT_ITEM=0 = Xbox menu).
    src/hle_frontend.c
    # Characters taken out of the world, never drawn (RECOMP_HIDE_CHARACTERS=0 = drawn).
    src/hle_world.c
    src/profiler.c
    # Direct3D 8 on a host D3D12 renderer, ported from SCDA-XBERecomp
    # (src/scda, MIT): the XDK's D3D entry points instead of the NV2A.
    src/scda/xdk/hle_d3d8.cpp
    # All are hand-written over generated bodies: the recompile (run from hpps/) must pass
    #   --exclude-manual port/src/hle_dsound.c
    #   --exclude-manual port/src/hle_video.c
    #   --exclude-manual port/src/hle_input.c
    #   --exclude-manual port/src/hle_xapi.c
    #   --exclude-manual port/src/hle_debug.c
    #   --exclude-manual port/src/hle_anim.c
    #   --exclude-manual port/src/hle_trainer.c
    #   --exclude-manual port/src/hle_frontend.c
    #   --exclude-manual port/src/hle_world.c
    #   --exclude-manual port/src/scda/xdk/hle_d3d8.cpp
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

# No game art in the exes, in any build (2026-10-29): the icon is the port's own, res/app.ico (tools/make_icon.py ps: a
# wand drawn from shapes in the launcher's Gryffindor scarlet), no longer one made from the XBE's title image; the
# launcher shows the title as text over that same colour.
# HPPS_NO_ART: kept so tools/package_release.sh's -DHPPS_NO_ART=ON still configures; nothing depends on it now
option(HPPS_NO_ART "No game artwork in the exes (now always the case)" OFF)
set(HPPS_RC_ICON "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico\"\n")
# resource 1: the icon (fb_present.c and the launcher load it for their windows).
# Each also carries its version information (Explorer's Properties > Details), which keeps the file from being
# empty without art: windres refuses a resource script with no resources
# HPPS_VERSION: the release's version (tools/package_release.sh passes VERSION), in Properties > Details as the file
# and product version; the description is the product's name, the copyright the port's author
set(HPPS_VERSION "0.4.2" CACHE STRING "the release version, major.minor.patch")
string(REGEX MATCHALL "[0-9]+" hpps_ver_parts "${HPPS_VERSION}.0.0.0")
list(GET hpps_ver_parts 0 hpps_v0)
list(GET hpps_ver_parts 1 hpps_v1)
list(GET hpps_ver_parts 2 hpps_v2)
set(HPPS_PRODUCT "Harry Potter and the Philosopher's Stone - unofficial PC port")
function(hpps_version_rc out file)
    set(v "${hpps_v0},${hpps_v1},${hpps_v2},0")
    set(${out} "1 VERSIONINFO\nFILEVERSION ${v}\nPRODUCTVERSION ${v}\nFILEOS 0x40004\nFILETYPE 1\nBEGIN\n  BLOCK \"StringFileInfo\"\n  BEGIN\n    BLOCK \"040904B0\"\n    BEGIN\n      VALUE \"CompanyName\", \"MiddleDucky\"\n      VALUE \"FileDescription\", \"${HPPS_PRODUCT}\"\n      VALUE \"ProductName\", \"${HPPS_PRODUCT}\"\n      VALUE \"FileVersion\", \"${HPPS_VERSION}\"\n      VALUE \"ProductVersion\", \"${HPPS_VERSION}\"\n      VALUE \"LegalCopyright\", \"Copyright (C) 2026 MiddleDucky\"\n      VALUE \"OriginalFilename\", \"${file}\"\n      VALUE \"InternalName\", \"${file}\"\n    END\n  END\n  BLOCK \"VarFileInfo\"\n  BEGIN\n    VALUE \"Translation\", 0x409, 1200\n  END\nEND\n" PARENT_SCOPE)
endfunction()
hpps_version_rc(HPPS_RC_VERSION_GAME "hpps.exe")
hpps_version_rc(HPPS_RC_VERSION_LAUNCHER "hpps_launcher.exe")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hpps_res.rc"
     CONTENT "// generated by CMakeLists.txt\n${HPPS_RC_ICON}${HPPS_RC_VERSION_GAME}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/launcher_res.rc"
     CONTENT "// generated by CMakeLists.txt\n${HPPS_RC_ICON}${HPPS_RC_VERSION_LAUNCHER}")
# the icon is named by these scripts, not part of them: without this a new res/app.ico left the old one in the exes
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hpps_res.rc" "${CMAKE_CURRENT_BINARY_DIR}/launcher_res.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/hpps_res.rc
)

# MinGW GCC 16 at -O3 sometimes hoists the address of a thread local array out of a loop as a plain global: the
# lifted code then reads the TLS template instead of the thread's copy (g_fp_stack in sub_00036EC0, which turned the
# Whomping Willow's right hand to angle 0). -O2 keeps it out; the post build check below fails on any such access.
if(WIN32)
    if(HPPS_RELEASE)
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
            "-O2;-fno-stack-protector;-U_FORTIFY_SOURCE")
    else()
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_tls_refs.py
                    --objdump ${CMAKE_OBJDUMP} --nm ${CMAKE_NM} $<TARGET_FILE:${PROJECT_NAME}>
            VERBATIM)
    endif()
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# SCDA-XBERecomp's renderer (src/scda/gpu): XDK Direct3D state to D3D12.
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE
        src/scda/app/app.cpp
        src/scda/gpu/d3d12.cpp
        src/scda/gpu/pipeline_cache.cpp
        src/scda/gpu/render_thread.cpp
        src/scda/gpu/shaders.cpp
        src/scda/gpu/texture.cpp
        src/scda/gpu/vertex.cpp
        src/scda/app/menu.cpp
        src/scda/app/input.cpp
    )
    # Dear ImGui v1.91.0 (third_party/imgui, MIT), as SCDA: the F10 options panel
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    target_sources(${PROJECT_NAME} PRIVATE
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
    set_source_files_properties(
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${IMGUI_DIR}" COMPILE_OPTIONS "-O2")
    set_source_files_properties(
        src/scda/xdk/hle_d3d8.cpp src/scda/app/app.cpp src/scda/gpu/d3d12.cpp
        src/scda/gpu/pipeline_cache.cpp src/scda/gpu/render_thread.cpp src/scda/gpu/shaders.cpp
        src/scda/gpu/texture.cpp src/scda/gpu/vertex.cpp src/scda/app/menu.cpp src/scda/app/input.cpp
        PROPERTIES INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/scda;${XBOXRECOMP_DIR}/src;${IMGUI_DIR}"
        COMPILE_OPTIONS "-msse4.1")
    set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

if(WIN32)
    # the C++ renderer must not need MinGW's libstdc++/libgcc/winpthread DLLs at run time
    target_link_options(hpps PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_libraries(hpps PRIVATE d3d11 d3d12 d3dcompiler dxgi dxguid xinput1_4 winmm dbghelp xaudio2_8 ole32 imm32 dwmapi gdi32)
else()
    target_link_options(hpps PRIVATE -rdynamic)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# HPPS_CALL_PROFILE: count every direct call by target (F7 dumps calls_N.txt, see profiler.c)
option(HPPS_CALL_PROFILE "Count direct calls per target, F7 dumps" OFF)
if(HPPS_CALL_PROFILE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_CALL_PROFILE)
endif()

# HPPS_DIFFTEST: compare every called function with its original x86 in Unicorn (F5 sweep, difftest.txt; see
# src/difftest.c). Needs unicorn.dll next to the executable.
option(HPPS_DIFFTEST "Differential test against the original x86 (Unicorn)" OFF)
if(HPPS_DIFFTEST)
    target_sources(${PROJECT_NAME} PRIVATE src/difftest.c)
    target_include_directories(${PROJECT_NAME} PRIVATE third_party/unicorn)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_DIFFTEST)
endif()

# hpps_launcher.exe: 4:3 or 16:9 and the graphics settings, then hpps.exe (src/launcher/launcher.cpp)
if(WIN32)
    add_executable(hpps_launcher WIN32
        src/launcher/launcher.cpp
        src/launcher/xiso.c  # unpacks a disc image in game\ before the first start
        src/launcher/gamefiles.c  # checks game\ against the disc's files (game_files.inc)
        ${CMAKE_CURRENT_BINARY_DIR}/launcher_res.rc
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(hpps_launcher PRIVATE ${IMGUI_DIR})
    set_target_properties(hpps_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_compile_options(hpps_launcher PRIVATE -O2)
    target_link_options(hpps_launcher PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
    target_link_libraries(hpps_launcher PRIVATE d3d11 dxgi d3dcompiler ole32 imm32 dwmapi gdi32 xinput1_4)
endif()

# hpps_trainer.exe: the debug trainer beside the game (noclip, spells, health, saved spots); src/trainer/trainer.c,
# the game side is src/hle_trainer.c
if(WIN32)
    add_executable(hpps_trainer WIN32 src/trainer/trainer.c)
    target_compile_options(hpps_trainer PRIVATE -O2)
    target_link_options(hpps_trainer PRIVATE -static -static-libgcc -mwindows)
    target_link_libraries(hpps_trainer PRIVATE comctl32 gdi32 user32)
endif()
