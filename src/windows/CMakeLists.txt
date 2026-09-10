if(NOT WIN32)
  return()
endif()

add_library(ht2mp_windows_runtime STATIC runtime.cpp)
add_library(ht2mp::windows_runtime ALIAS ht2mp_windows_runtime)

target_include_directories(ht2mp_windows_runtime PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(ht2mp_windows_runtime PUBLIC cxx_std_20)
target_link_libraries(ht2mp_windows_runtime PUBLIC bcrypt shell32 ole32)

if(MSVC)
  target_compile_options(ht2mp_windows_runtime PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
endif()
