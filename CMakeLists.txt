cmake_minimum_required(VERSION 3.22)
project(RadekiOSNative CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(radeki STATIC mach_o/macho.cpp binary_analysis/analysis.cpp objc/objc.cpp loader/dyld.cpp
                            relinker/relinker.cpp runtime/runtime.cpp runtime/compat_libsystem.cpp runtime/compat_cxx.cpp
                            runtime/cxx_forward.cpp runtime/stub_dispatch.cpp runtime/framework_stubs.cpp
                            compat/status.cpp compat/matrix.cpp ipa/inflate.cpp ipa/png.cpp ipa/plist.cpp ipa/bundle.cpp)
target_include_directories(radeki PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(radeki PRIVATE -Wall -Wextra)
# Android-only runtime shims use liblog, dlopen/dlsym and ANativeWindow buffer APIs; link their
# owning system libraries explicitly because the NDK does not add them to every consumer.
if(ANDROID)
  target_link_libraries(radeki PUBLIC log dl android)
  if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_link_libraries(radeki PUBLIC atomic)
  endif()
endif()
add_executable(radeki_cli tools/radeki.cpp)
set_target_properties(radeki_cli PROPERTIES OUTPUT_NAME radeki)
target_link_libraries(radeki_cli radeki)
if(NOT ANDROID)
  add_executable(radeki_tests tests/test_main.cpp)
  target_include_directories(radeki_tests PRIVATE tests)
  target_link_libraries(radeki_tests radeki)
  enable_testing()
  add_test(NAME radeki_tests COMMAND radeki_tests)
endif()
