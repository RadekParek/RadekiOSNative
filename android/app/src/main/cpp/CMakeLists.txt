cmake_minimum_required(VERSION 3.22)
project(radekijni CXX)
set(CMAKE_CXX_STANDARD 20)
set(R ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
add_library(radekijni SHARED radeki_jni.cpp ${R}/mach_o/macho.cpp ${R}/binary_analysis/analysis.cpp
                            ${R}/objc/objc.cpp ${R}/loader/dyld.cpp ${R}/relinker/relinker.cpp
                            ${R}/runtime/runtime.cpp ${R}/runtime/compat_libsystem.cpp ${R}/runtime/compat_cxx.cpp
                            ${R}/runtime/cxx_forward.cpp ${R}/runtime/stub_dispatch.cpp ${R}/runtime/framework_stubs.cpp
                            ${R}/compat/status.cpp ${R}/compat/matrix.cpp ${R}/ipa/inflate.cpp ${R}/ipa/png.cpp ${R}/ipa/plist.cpp ${R}/ipa/bundle.cpp)
target_include_directories(radekijni PRIVATE ${R} ${R}/tests)
target_link_libraries(radekijni log dl EGL GLESv2 android)
