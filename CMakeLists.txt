cmake_minimum_required(VERSION 3.24)
project(TaskbarCustomizer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Taskbar Customizer requires Windows x64 and MSVC.")
endif()

enable_language(RC)
add_executable(TaskbarCustomizer WIN32 src/main.cpp src/xaml_client.cpp src/startup.cpp src/settings.cpp src/app.rc)
target_include_directories(TaskbarCustomizer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_library(TaskbarCustomizerXaml SHARED src/xaml_tap.cpp src/xaml_tap.def)
add_dependencies(TaskbarCustomizer TaskbarCustomizerXaml)

foreach(target IN ITEMS TaskbarCustomizer TaskbarCustomizerXaml)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")
  set_target_properties(${target} PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

target_link_libraries(TaskbarCustomizer PRIVATE user32 shell32 advapi32 ole32 uuid windowsapp)
target_link_libraries(TaskbarCustomizerXaml PRIVATE user32 ole32 oleaut32 windowsapp)
target_link_options(TaskbarCustomizer PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
