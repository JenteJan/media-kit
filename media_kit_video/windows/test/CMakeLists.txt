cmake_minimum_required(VERSION 3.14)
project(media_kit_windows_tests LANGUAGES CXX)
enable_testing()
if(WIN32)
  add_executable(platform_task_runner_test platform_task_runner_test.cpp
    ../platform_task_runner.cpp)
  target_compile_features(platform_task_runner_test PRIVATE cxx_std_17)
  target_compile_definitions(platform_task_runner_test PRIVATE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR UNICODE _UNICODE)
  target_compile_options(platform_task_runner_test PRIVATE /W4 /WX /EHsc)
  add_test(NAME platform_task_runner COMMAND platform_task_runner_test)
endif()
add_executable(video_frame_size_test video_frame_size_test.cpp)
target_compile_features(video_frame_size_test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(video_frame_size_test PRIVATE /W4 /WX /EHsc)
else()
  target_compile_options(video_frame_size_test PRIVATE -Wall -Wextra -Werror)
endif()

# Test the output and its manager with simulated libmpv failures.
set(MEDIA_KIT_FLUTTER_DIR "" CACHE PATH "Flutter Windows ephemeral directory")
set(MEDIA_KIT_LIBMPV_DIR "" CACHE PATH "libmpv directory from the Flutter Windows build")
if(WIN32 AND MEDIA_KIT_FLUTTER_DIR AND MEDIA_KIT_LIBMPV_DIR)
  add_executable(video_output_lifecycle_test video_output_lifecycle_test.cpp
    ../video_output.cpp ../video_output_manager.cpp ../d3d11_surface_manager.cpp)
  target_compile_features(video_output_lifecycle_test PRIVATE cxx_std_17)
  target_compile_definitions(video_output_lifecycle_test PRIVATE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR UNICODE _UNICODE)
  target_include_directories(video_output_lifecycle_test PRIVATE
    "${MEDIA_KIT_FLUTTER_DIR}" "${MEDIA_KIT_FLUTTER_DIR}/cpp_client_wrapper/include"
    "${MEDIA_KIT_LIBMPV_DIR}/include/mpv")
  target_link_libraries(video_output_lifecycle_test PRIVATE d3d11 dxgi)
  add_test(NAME video_output_lifecycle COMMAND video_output_lifecycle_test)
  add_test(NAME notify_render_queue COMMAND video_output_lifecycle_test --render-queue)
  set_tests_properties(video_output_lifecycle notify_render_queue PROPERTIES TIMEOUT 30)
endif()
add_test(NAME video_frame_size COMMAND video_frame_size_test)

if(WIN32)
  add_executable(texture_generation_test texture_generation_test.cpp
    ../d3d11_surface_manager.cpp)
  target_compile_features(texture_generation_test PRIVATE cxx_std_17)
  target_compile_definitions(texture_generation_test PRIVATE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR UNICODE _UNICODE)
  target_link_libraries(texture_generation_test PRIVATE d3d11 dxgi)
  add_test(NAME texture_generation COMMAND texture_generation_test)
  set_tests_properties(texture_generation PROPERTIES TIMEOUT 60)
endif()
