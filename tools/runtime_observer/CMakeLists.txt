if(NOT WIN32)
  return()
endif()

add_executable(ht2mp-runtime-observer main.cpp)
target_compile_features(ht2mp-runtime-observer PRIVATE cxx_std_20)
target_link_libraries(ht2mp-runtime-observer PRIVATE ht2mp_game)

if(MSVC)
  target_compile_options(ht2mp-runtime-observer PRIVATE
    /W4 /permissive- /Zc:__cplusplus /FS
  )
  target_compile_definitions(ht2mp-runtime-observer PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
  )
endif()
