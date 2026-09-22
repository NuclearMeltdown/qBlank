cmake_minimum_required(VERSION 3.21)
project(qBlank LANGUAGES CXX RC VERSION 4.4.1)

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
  src/main.cpp
  src/app_files.cpp
  src/app_files_win32.cpp
  src/app_identity.cpp
  src/common.cpp
  src/common_win32.cpp
  src/text_win32.cpp
  src/wake_signal_win32.cpp
  src/i18n.cpp
  src/app.cpp
  src/config.cpp
  src/hotkeys.cpp
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
  src/audio/audio_engine.cpp
  src/audio/audio_ring.cpp
  src/audio/dshow_audio_capture.cpp
  src/audio/mic_capture.cpp
  src/audio/pcm.cpp
  src/record/ffmpeg_download.cpp
  src/record/ffmpeg_locator.cpp
  src/record/recorder.cpp
  src/record/remuxer.cpp
  src/record/screenshot.cpp
  src/render/d3d_context.cpp
  src/render/video_renderer.cpp
  src/ui/file_dialog.cpp
  src/ui/toolbar.cpp
  src/ui/theme.cpp
  src/update/release_source.cpp
  src/update/updater.cpp
  src/vcam/virtual_camera.cpp
  src/ui/settings_host.cpp
  src/ui/settings_search.cpp
  src/ui/settings_window.cpp
  src/ui/startup_dialog.cpp
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
  src/text_win32.cpp
  src/wake_signal_win32.cpp
  src/i18n.cpp
  src/config.cpp
  src/hotkeys.cpp
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
)
target_include_directories(qblank_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(qblank_probe PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(qblank_probe PRIVATE
  strmiids ole32 oleaut32 uuid dxguid user32 advapi32 shlwapi propsys avrt
  mfplat mf mfuuid mfreadwrite)
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
