cmake_minimum_required(VERSION 3.24)
project(sims4vr CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Sims 4 is 64-bit only: configure with -A x64")
endif()

set(OPENXR_VERSION 1.1.63)

include(FetchContent)

# Headers only: SOURCE_SUBDIR points nowhere so the SDK's own build is skipped.
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-${OPENXR_VERSION}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR headers-only)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR headers-only)
# Dear ImGui (MIT): our own HUD (hud.cpp). Sources compiled in below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(openxr minhook imgui)

# Prebuilt Windows loader, shipped next to sims4vr.dll.
set(LOADER_ZIP ${CMAKE_BINARY_DIR}/openxr_loader_windows-${OPENXR_VERSION}.zip)
set(LOADER_DIR ${CMAKE_BINARY_DIR}/openxr_loader)
if(NOT EXISTS ${LOADER_DIR}/x64/bin/openxr_loader.dll)
    file(DOWNLOAD
        https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-${OPENXR_VERSION}/openxr_loader_windows-${OPENXR_VERSION}.zip
        ${LOADER_ZIP} SHOW_PROGRESS STATUS LOADER_STATUS)
    file(ARCHIVE_EXTRACT INPUT ${LOADER_ZIP} DESTINATION ${LOADER_DIR})
endif()

add_library(sims4vr SHARED
    src/addrs.cpp
    src/api.cpp
    src/armik.cpp
    src/handshape.cpp
    src/camera.cpp
    src/player.cpp
    src/camfind.cpp
    src/config.cpp
    src/crashlog.cpp
    src/gamefix.cpp
    src/ambience.cpp
    src/mirrorcap.cpp
    src/shadows.cpp
    src/texbias.cpp
    src/shadowreuse.cpp
    src/passtime.cpp
    src/doorswing.cpp
    src/walkstop.cpp
    src/eyelids.cpp
    src/facehide.cpp
    src/framecost.cpp
    src/hitch.cpp
    src/simcache.cpp
    src/texprobe.cpp
    src/cpuprio.cpp
    src/benchmark.cpp
    src/framemap.cpp
    src/singleview.cpp
    src/deptheye.cpp
    src/touch.cpp
    src/grab.cpp
    src/d3d9_hook.cpp
    src/d3d11_hook.cpp
    src/log.cpp
    src/modelbox.cpp
    src/strings.cpp
    src/pymodule.cpp
    src/pointer.cpp
    src/gamepad.cpp
    src/gpuload.cpp
    src/othermods.cpp
    src/nearplane.cpp
    src/uihook.cpp
    src/bubbles.cpp
    src/nodim.cpp
    src/phone3d.cpp
    src/trail.cpp
    src/renderscale.cpp
    src/gameaddr.cpp
    src/rtti.cpp
    src/screenshot.cpp
    src/settings.cpp
    src/simsforever.cpp
    src/claudescreen.cpp
    src/simsforever_sound.cpp
    src/sharpen.cpp
    src/fsr.cpp
    src/bodylayer.cpp
    src/eyealign.cpp
    src/vr.cpp
    src/voice.cpp
    src/hud.cpp
    src/uikit.cpp
    src/thumbs.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(sims4vr PRIVATE
    ${openxr_SOURCE_DIR}/include
    ${minhook_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
# The DirectX 11 backend's shader compiler comes from d3dcompiler_47.dll at
# runtime (hud.cpp), like bodylayer.cpp's, not from an import library.
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp PROPERTIES
    COMPILE_DEFINITIONS "D3DCompile=s4vr_D3DCompile"
    COMPILE_OPTIONS "-w")
# The game's own UI art the HUD draws (tools/pack_hud_art.py), embedded with .incbin.
set_source_files_properties(src/hud.cpp PROPERTIES
    COMPILE_DEFINITIONS "S4VR_HUD_ART=\"${CMAKE_CURRENT_SOURCE_DIR}/art/hud_art.bin\""
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/art/hud_art.bin)
set_source_files_properties(${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp PROPERTIES COMPILE_OPTIONS "-w")
# Set by tools/release.sh (-DS4VR_BUILD_VERSION=0.13.270 -DS4VR_BUILD_COMMIT=abc1234); first line of sims4vr.log.
set(S4VR_BUILD_VERSION "dev" CACHE STRING "Version string written to the log")
set(S4VR_BUILD_COMMIT "none" CACHE STRING "Git commit written to the log")
target_compile_definitions(sims4vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    S4VR_BUILD_VERSION="${S4VR_BUILD_VERSION}" S4VR_BUILD_COMMIT="${S4VR_BUILD_COMMIT}")
# No d3d11: its functions come from System32 at runtime (d3d11_hook.cpp), so a d3d11.dll
# overlay in the game folder is never loaded on the mod's account.
target_link_libraries(sims4vr PRIVATE dxgi ole32 windowscodecs gdi32 pdh)
if(MSVC)
    target_compile_options(sims4vr PRIVATE /W4 /permissive-)
    set_property(TARGET sims4vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # {XR_TYPE_...} initialisers and GetProcAddress casts are idiomatic here
    target_compile_options(sims4vr PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
    # The map file turns crash-report offsets in sims4vr.dll back into function names.
    target_link_options(sims4vr PRIVATE -static -s -Wl,-Map=${CMAKE_BINARY_DIR}/sims4vr.map)
    set_target_properties(sims4vr PROPERTIES PREFIX "")
endif()

# build/package/sims4vr/ = the folder that goes into Documents/Electronic Arts/The Sims 4/Mods/
set(PACKAGE_DIR ${CMAKE_BINARY_DIR}/package/sims4vr)
add_custom_command(TARGET sims4vr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sims4vr> ${PACKAGE_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy ${LOADER_DIR}/x64/bin/openxr_loader.dll ${PACKAGE_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config/sims4vr.ini ${PACKAGE_DIR}/
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/build_nofade.py ${CMAKE_SOURCE_DIR}/data/fade_default.simdata
            ${CMAKE_SOURCE_DIR}/data/fade_minspec.simdata ${CMAKE_SOURCE_DIR}/data/dof_default.simdata
            ${PACKAGE_DIR}/sims4vr_nofade.package
    COMMAND ${CMAKE_COMMAND} -E touch ${PACKAGE_DIR}/sims4vr_probe.py)

# Sims Forever's 32-bit parts (Sims 1 is 32-bit): sf_launch.exe and sf_hook.dll.
find_program(I686_GXX i686-w64-mingw32-g++)
if(I686_GXX)
    add_custom_command(TARGET sims4vr POST_BUILD
        COMMAND sh ${CMAKE_SOURCE_DIR}/tools/build_simsforever.sh ${PACKAGE_DIR})
else()
    message(WARNING "No i686-w64-mingw32-g++: Sims Forever's sf_launch.exe / sf_hook.dll are not built")
endif()

# Stand-in for the game for testing the headset pipeline (see tools/testhost.cpp).
add_executable(sims4vr_testhost tools/testhost.cpp)
target_compile_definitions(sims4vr_testhost PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(sims4vr_testhost PRIVATE d3d11 dxgi)
if(NOT MSVC)
    target_link_options(sims4vr_testhost PRIVATE -static -s)
endif()
add_custom_command(TARGET sims4vr_testhost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sims4vr_testhost> ${PACKAGE_DIR}/)
