cmake_minimum_required(VERSION 3.21)
project(qBlank LANGUAGES CXX RC VERSION 5.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The version number has one home: project(... VERSION) above. From there it
# reaches the C++ side as QBLANK_VERSION and the Windows version resource
# through res/qblank.rc.in, so a release bumps one number in one file. It used
# to be two, and the second one drifted -- 3.5.1 shipped an executable that told
# Explorer it was 3.5.0.0.
add_compile_definitions(QBLANK_VERSION="${PROJECT_VERSION}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/qblank.rc.in"
               "${CMAKE_BINARY_DIR}/qblank.rc" @ONLY)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

# The virtual camera's DirectShow source filter. A DLL of its own because it
# does not run in qBlank: DirectShow loads it into whichever application opened
# the camera -- see src/vcam/vcam_shared.h. Registration goes to HKLM, so
# installing the camera needs administrator rights while using it does not.
add_library(qblank_vcam SHARED
  src/vcam/vcam_dll.cpp
  src/vcam/vcam_filter.cpp
  src/vcam/vcam_idle.cpp
  src/vcam/qblank_vcam.def
  res/vcam_dll.rc
)
target_include_directories(qblank_vcam PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/res")
target_compile_definitions(qblank_vcam PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
# gdi32 and user32 draw the idle picture -- see src/vcam/vcam_idle.cpp.
target_link_libraries(qblank_vcam PRIVATE
  strmiids ole32 oleaut32 uuid advapi32 gdi32 user32)
if(MSVC)
  target_compile_options(qblank_vcam PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8 /MP)
  set_property(TARGET qblank_vcam PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_target_properties(qblank_vcam PROPERTIES
  PREFIX ""
  OUTPUT_NAME "qblank_vcam"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)

add_executable(qBlank WIN32
  src/main_win32.cpp
  src/program.cpp
  src/app_files.cpp
  src/app_files_win32.cpp
  src/app_identity.cpp
  src/common.cpp
  src/common_win32.cpp
  src/platform_win32.cpp
  src/desktop_win32.cpp
  src/tray_win32.cpp
  src/child_process_win32.cpp
  src/http_win32.cpp
  src/files.cpp
  src/files_win32.cpp
  src/archive_win32.cpp
  src/text_win32.cpp
  src/wake_signal_win32.cpp
  src/i18n.cpp
  src/app.cpp
  src/app_frame.cpp
  src/app_hosts.cpp
  src/app_input.cpp
  src/app_source.cpp
  src/app_tray.cpp
  src/app_ui.cpp
  src/video_standard_search.cpp
  src/recording_control.cpp
  src/crop_tool.cpp
  src/config.cpp
  src/hotkeys.cpp
  src/keys.cpp
  src/keys_win32.cpp
  src/window_win32.cpp
  src/json.cpp
  src/capture/device_config.cpp
  src/capture/caps_model.cpp
  src/capture/capture_device_win32.cpp
  src/capture/dshow_util.cpp
  src/capture/frame_buffer.cpp
  src/capture/frame_sink.cpp
  src/capture/video_capture.cpp
  src/capture/video_format.cpp
  src/capture/video_standard.cpp
  src/audio/audio_devices.cpp
  src/audio/audio_devices_win32.cpp
  src/audio/audio_engine.cpp
  src/audio/audio_ring.cpp
  src/audio/audio_stream_win32.cpp
  src/audio/dshow_audio_capture.cpp
  src/audio/mic_capture.cpp
  src/audio/pcm.cpp
  src/audio/pcm_win32.cpp
  src/record/ffmpeg_download.cpp
  src/record/ffmpeg_locator.cpp
  src/record/recorder.cpp
  src/record/remuxer.cpp
  src/record/screenshot.cpp
  src/render/display.cpp
  src/render/display_win32.cpp
  src/render/render_passes.cpp
  src/render/render_passes_win32.cpp
  src/render/video_renderer.cpp
  src/ui/file_dialog.cpp
  src/ui/file_dialog_win32.cpp
  src/ui/ui_surface_win32.cpp
  src/ui/toolbar.cpp
  src/ui/theme.cpp
  src/update/release_source.cpp
  src/update/updater.cpp
  src/camera_sink_win32.cpp
  src/vcam/virtual_camera.cpp
  src/ui/settings_host.cpp
  src/ui/tray_menu.cpp
  src/ui/settings_search.cpp
  src/ui/settings_window.cpp
  src/ui/startup_dialog.cpp
  src/ui/startup_notices.cpp
  src/ui/overlay.cpp

  "${CMAKE_BINARY_DIR}/qblank.rc"

  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(qBlank PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${CMAKE_CURRENT_SOURCE_DIR}/res"
)

target_compile_definitions(qBlank PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)

# The shaders go into the executable without their comments -- see
# tools/strip_shader_comments.cmake. The copy is made when CMake configures, and
# a change to shaders.h makes it configure again.
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/strip_shader_comments.cmake")
qblank_strip_comments("${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders.h"
                      "${CMAKE_BINARY_DIR}/generated/shaders_nocomments.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders.h"
             "${CMAKE_CURRENT_SOURCE_DIR}/tools/strip_shader_comments.cmake")
target_include_directories(qBlank PRIVATE "${CMAKE_BINARY_DIR}/generated")

# ...and packed -- see src/inflate.h. A small program writes each shader out as
# a file, and tools/embed_packed.cmake deflates the files into a header, which
# Direct3D 11 unpacks its source from when it starts. The program includes the
# copy without comments, so a change to shaders.h rebuilds it and everything
# after it. The Vulkan renderer below compiles the same files to SPIR-V.
add_executable(qblank_hlsl_dump src/tools/hlsl_dump.cpp)
target_include_directories(qblank_hlsl_dump PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(qblank_hlsl_dump PRIVATE
  QBLANK_SHADERS_WITHOUT_COMMENTS _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(qblank_hlsl_dump PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8)
  set_property(TARGET qblank_hlsl_dump PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# file : dxc profile : its source in shaders_hlsl.h : its SPIR-V in shaders_spirv.h
set(qblank_shaders
  fullscreen_vs:vs_6_0:kFullscreenVS:kSpirvFullscreenVS
  clean_ps:ps_6_0:kCleanPS:kSpirvClean
  convert_ps:ps_6_0:kConvertPS:kSpirvConvert
  scale_ps:ps_6_0:kScalePS:kSpirvScale
  hdr_record_ps:ps_6_0:kHdrRecordPS:kSpirvHdrRecord
  ui_composite_ps:ps_6_0:kUiCompositePS:kSpirvUiComposite
)
set(shader_dir "${CMAKE_BINARY_DIR}/shaders")
set(hlsl_files)
set(hlsl_names)
foreach(shader IN LISTS qblank_shaders)
  string(REPLACE ":" ";" parts "${shader}")
  list(GET parts 0 file)
  list(GET parts 2 name)
  list(APPEND hlsl_files "${shader_dir}/${file}.hlsl")
  list(APPEND hlsl_names "${file}.hlsl=${name}")
endforeach()
add_custom_command(
  OUTPUT ${hlsl_files}
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
  COMMAND qblank_hlsl_dump "${shader_dir}"
  DEPENDS qblank_hlsl_dump
  COMMENT "Writing the shaders out"
  VERBATIM)

string(REPLACE ";" "," hlsl_names "${hlsl_names}")
set(hlsl_header "${CMAKE_BINARY_DIR}/generated/shaders_hlsl.h")
add_custom_command(
  OUTPUT "${hlsl_header}"
  COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${hlsl_header}" "-DDIR=${shader_dir}"
          "-DFILES=${hlsl_names}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_packed.cmake"
  DEPENDS ${hlsl_files} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_packed.cmake"
  COMMENT "Packing the shaders"
  VERBATIM)
target_sources(qBlank PRIVATE src/inflate.cpp "${hlsl_header}")
target_compile_definitions(qBlank PRIVATE QBLANK_SHADERS_PACKED)

# The Vulkan renderer, next to Direct3D 11 -- see src/render/display_vulkan.cpp.
# Which of the two runs is a setting; Direct3D 11 stays the default and the
# fallback when Vulkan does not start.
#
# Building it needs the Vulkan SDK: the headers, the loader's import library and
# a dxc that writes SPIR-V. Without them the build goes on with Direct3D 11
# alone and the setting does not appear -- with a warning, because a build that
# quietly lost a renderer looks exactly like one that has it.
# -DQBLANK_VULKAN=OFF leaves it out on purpose.
option(QBLANK_VULKAN "Build the Vulkan renderer (needs the Vulkan SDK)" ON)
set(qblank_vulkan_ready OFF)
if(QBLANK_VULKAN)
  find_package(Vulkan)
  # The SDK's own dxc first: the one in the Windows SDK, which a developer
  # prompt puts on PATH, is built without SPIR-V.
  set(qblank_dxc_hints "$ENV{VULKAN_SDK}/Bin")
  if(Vulkan_INCLUDE_DIR)
    list(APPEND qblank_dxc_hints "${Vulkan_INCLUDE_DIR}/../Bin")
  endif()
  find_program(QBLANK_DXC dxc HINTS ${qblank_dxc_hints})

  if(NOT Vulkan_FOUND)
    message(WARNING "Vulkan SDK not found: building with Direct3D 11 only")
  elseif(NOT QBLANK_DXC)
    message(WARNING "dxc not found in the Vulkan SDK: building with Direct3D 11 only")
  else()
    # Found is not enough -- it has to write SPIR-V, so it is asked to.
    set(probe "${CMAKE_BINARY_DIR}/dxc_probe")
    file(WRITE "${probe}.hlsl" "float4 main() : SV_Target { return 0; }\n")
    execute_process(
      COMMAND "${QBLANK_DXC}" -spirv -T ps_6_0 -E main "${probe}.hlsl" -Fo "${probe}.spv"
      RESULT_VARIABLE probe_result OUTPUT_QUIET ERROR_QUIET)
    if(probe_result EQUAL 0 AND EXISTS "${probe}.spv")
      set(qblank_vulkan_ready ON)
    else()
      message(WARNING "${QBLANK_DXC} does not write SPIR-V: building with Direct3D 11 only")
    endif()
    file(REMOVE "${probe}.hlsl" "${probe}.spv")
  endif()
endif()

if(qblank_vulkan_ready)
  message(STATUS "Vulkan renderer: on (dxc: ${QBLANK_DXC})")

  # The shaders are the ones Direct3D 11 compiles at startup, from the files
  # written out above, compiled to SPIR-V here instead -- and packed the same way.
  #
  # The flags keep the shaders what they are under Direct3D 11:
  #   -HV 2018          the HLSL version fxc speaks, including its scoping of a
  #                     for loop's variable -- which is also why the warning
  #                     about a loop redeclaring one is off: two loops in a row
  #                     each declaring their own counter is what was meant.
  #   -fvk-invert-y     clip space points up under Direct3D and down under
  #                     Vulkan; the vertex shader turns it around.
  #   -fvk-?-shift      textures, samplers and constant buffers share the
  #                     register numbers t0, s0, b0 in HLSL and would collide
  #                     as Vulkan bindings, so each kind gets its own range:
  #                     textures from 0, samplers from 16, buffers from 32 --
  #                     see src/render/render_passes_vulkan.cpp.
  set(qblank_dxc_flags -spirv -HV 2018 -O3 -E main -fspv-target-env=vulkan1.1
      -fvk-t-shift 0 0 -fvk-s-shift 16 0 -fvk-b-shift 32 0 -Wno-for-redefinition)
  set(spirv_files)
  set(spirv_names)
  foreach(shader IN LISTS qblank_shaders)
    string(REPLACE ":" ";" parts "${shader}")
    list(GET parts 0 file)
    list(GET parts 1 profile)
    list(GET parts 3 name)
    set(stage_flags)
    if(profile MATCHES "^vs_")
      set(stage_flags -fvk-invert-y)
    endif()
    add_custom_command(
      OUTPUT "${shader_dir}/${file}.spv"
      COMMAND "${QBLANK_DXC}" ${qblank_dxc_flags} -T ${profile} ${stage_flags}
              "${shader_dir}/${file}.hlsl" -Fo "${shader_dir}/${file}.spv"
      DEPENDS "${shader_dir}/${file}.hlsl"
      COMMENT "Compiling ${file} to SPIR-V"
      VERBATIM)
    list(APPEND spirv_files "${shader_dir}/${file}.spv")
    list(APPEND spirv_names "${file}.spv=${name}")
  endforeach()

  string(REPLACE ";" "," spirv_names "${spirv_names}")
  set(spirv_header "${CMAKE_BINARY_DIR}/generated/shaders_spirv.h")
  add_custom_command(
    OUTPUT "${spirv_header}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${spirv_header}" "-DDIR=${shader_dir}"
            "-DFILES=${spirv_names}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_packed.cmake"
    DEPENDS ${spirv_files} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_packed.cmake"
    COMMENT "Packing the SPIR-V"
    VERBATIM)

  target_sources(qBlank PRIVATE
    src/render/display_vulkan.cpp
    src/render/display_vulkan_win32.cpp
    src/render/render_passes_vulkan.cpp
    src/render/vulkan_device.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    "${spirv_header}"
  )
  target_compile_definitions(qBlank PRIVATE QBLANK_VULKAN)
  target_link_libraries(qBlank PRIVATE Vulkan::Vulkan)
  # vulkan-1.dll comes with the graphics driver, not with Windows, and a
  # machine without it must still start qBlank on Direct3D 11. So the DLL is
  # loaded on first use rather than with the executable, and nothing uses it
  # before src/render/display_vulkan_win32.cpp has checked that it is there.
  if(MSVC)
    target_link_libraries(qBlank PRIVATE delayimp)
    target_link_options(qBlank PRIVATE /DELAYLOAD:vulkan-1.dll)
  endif()
endif()

target_link_libraries(qBlank PRIVATE
  d3d11 dxgi d3dcompiler dxguid
  strmiids ole32 oleaut32 uuid
  winmm avrt dwmapi shlwapi shcore winhttp bcrypt windowscodecs
  user32 gdi32 advapi32 comdlg32 propsys
)

if(MSVC)
  target_compile_options(qBlank PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8 /MP)
  target_compile_options(qBlank PRIVATE $<$<CONFIG:Release>:/O2 /Oi /GL>)
  target_link_options(qBlank PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
  # Static CRT so the exe runs without redistributables.
  set_property(TARGET qBlank PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_options(qBlank PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()

# The filter rides inside the executable as a resource, and the executable lays
# it down when the camera is installed. Shipping it as a loose second file meant
# the two could be from different builds -- which happened, because Windows locks
# the file while the camera is in use and replacing it failed silently.
set(VCAM_DLL_PATH "${CMAKE_BINARY_DIR}/bin/qblank_vcam.dll")
file(TO_NATIVE_PATH "${VCAM_DLL_PATH}" VCAM_DLL_NATIVE)
string(REPLACE "\\" "\\\\" VCAM_DLL_ESCAPED "${VCAM_DLL_NATIVE}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/vcam_resource.rc.in"
               "${CMAKE_BINARY_DIR}/vcam_resource.rc" @ONLY)
# So the resource is rebuilt when the source changes rather than when the path does.
set_source_files_properties("${CMAKE_BINARY_DIR}/vcam_resource.rc"
                            PROPERTIES OBJECT_DEPENDS "${VCAM_DLL_PATH}")
target_sources(qBlank PRIVATE "${CMAKE_BINARY_DIR}/vcam_resource.rc")
add_dependencies(qBlank qblank_vcam)

# The bridge out of the old name -- see src/migrate/migrator.cpp.
#
# Built under the file name the program had before the rename, because that is
# how it is found: the updater in CapView 3.7 looks for a release asset called
# exactly "CapView.exe" and starts what it downloads. So this one is uploaded
# under that name, in every release from now on, and the same binary is dragged
# from release to release without being touched -- it takes the new name from
# the release itself and never has to be told one.
#
# Its own output directory, so that nothing which copies "the build" out of
# bin/ picks up an executable under the old name by accident.
#
# One thing 3.7 does check is the size: anything under 256 KB is rejected as
# not-a-program, the guard that stops it writing an error page over itself. This
# target links the static runtime and comes out at roughly two and a half times
# that on its own, so nothing has to be padded -- but the floor is real, and a
# future attempt to make this file small would walk straight into it.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/migrator.rc.in"
               "${CMAKE_BINARY_DIR}/migrator.rc" @ONLY)

add_executable(CapViewMigrator WIN32
  src/migrate/migrator.cpp
  src/app_identity.cpp
  src/i18n.cpp
  src/json.cpp
  src/update/release_source.cpp
  src/http_win32.cpp
  "${CMAKE_BINARY_DIR}/migrator.rc"
)
target_include_directories(CapViewMigrator PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/res")
target_compile_definitions(CapViewMigrator PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(CapViewMigrator PRIVATE
  winhttp ole32 oleaut32 uuid shell32 shlwapi dwmapi uxtheme user32 gdi32 advapi32)
if(MSVC)
  target_compile_options(CapViewMigrator PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8 /MP)
  set_property(TARGET CapViewMigrator PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_options(CapViewMigrator PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()
set_target_properties(CapViewMigrator PROPERTIES
  OUTPUT_NAME "CapView"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/migrator"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/migrator"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/migrator"
)

# Diagnostic console tool: prints the devices, formats and inputs qBlank sees.
add_executable(qblank_probe
  src/tools/probe_main.cpp
  src/tools/mf_probe.cpp
  src/record/ffmpeg_locator.cpp
  src/app_files.cpp
  src/app_files_win32.cpp
  src/app_identity.cpp
  src/common.cpp
  src/common_win32.cpp
  src/platform_win32.cpp
  src/child_process_win32.cpp
  src/files.cpp
  src/files_win32.cpp
  src/text_win32.cpp
  src/wake_signal_win32.cpp
  src/i18n.cpp
  src/config.cpp
  src/hotkeys.cpp
  src/keys.cpp
  src/keys_win32.cpp
  src/json.cpp
  src/capture/caps_model.cpp
  src/capture/capture_device_win32.cpp
  src/capture/dshow_util.cpp
  src/capture/frame_buffer.cpp
  src/capture/frame_sink.cpp
  src/capture/video_capture.cpp
  src/capture/video_format.cpp
  src/capture/video_standard.cpp
  src/audio/audio_devices.cpp
  src/audio/audio_devices_win32.cpp
)
target_include_directories(qblank_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/res")
target_compile_definitions(qblank_probe PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qblank_probe PRIVATE
  strmiids ole32 oleaut32 uuid dxguid user32 advapi32 shlwapi propsys avrt
  winmm mfplat mf mfuuid mfreadwrite)
if(MSVC)
  target_compile_options(qblank_probe PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8)
  set_property(TARGET qblank_probe PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_target_properties(qblank_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(qBlank PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)

# Diagnostic console tool: a consumer for the virtual camera. Loads the filter
# DLL directly, so it needs neither an installation nor administrator rights,
# and reports what a real consumer would see -- offered formats, the negotiated
# one, the frame rate that actually arrived, and whether the picture was blank.
add_executable(qblank_vcam_test src/tools/vcam_test.cpp)
target_include_directories(qblank_vcam_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(qblank_vcam_test PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qblank_vcam_test PRIVATE strmiids ole32 oleaut32 uuid)
if(MSVC)
  target_compile_options(qblank_vcam_test PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8)
  set_property(TARGET qblank_vcam_test PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_target_properties(qblank_vcam_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)
add_dependencies(qblank_vcam_test qblank_vcam)
