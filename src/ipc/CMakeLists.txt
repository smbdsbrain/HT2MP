add_library(ht2mp_ipc STATIC authentication.cpp codec.cpp)
add_library(ht2mp::ipc ALIAS ht2mp_ipc)

target_include_directories(ht2mp_ipc PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(ht2mp_ipc PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(ht2mp_ipc PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
endif()
