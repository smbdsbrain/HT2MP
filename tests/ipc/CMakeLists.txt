add_executable(ht2mp-ipc-tests codec_tests.cpp)
target_compile_features(ht2mp-ipc-tests PRIVATE cxx_std_20)
target_link_libraries(ht2mp-ipc-tests PRIVATE ht2mp::ipc)

if(MSVC)
  target_compile_options(ht2mp-ipc-tests PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
endif()

add_test(NAME ipc-codec COMMAND ht2mp-ipc-tests)
