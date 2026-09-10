# Offline replay of a sidecar remote_states.csv trace through the bridge's
# RemotePoseTimeline. Emits the pose the bridge would have displayed at a fixed
# evaluation step so playback smoothness can be judged without a live game.
# The bridge runtime is only built for the x86 (game-side) preset.
if(NOT TARGET ht2mp_bridge_runtime)
  return()
endif()

add_executable(ht2mp-timeline-replay main.cpp)
target_compile_features(ht2mp-timeline-replay PRIVATE cxx_std_20)
target_include_directories(ht2mp-timeline-replay PRIVATE
  ${CMAKE_SOURCE_DIR}/bridge
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ht2mp-timeline-replay PRIVATE ht2mp_bridge_runtime)

if(MSVC)
  target_compile_options(ht2mp-timeline-replay PRIVATE
    /W4 /permissive- /Zc:__cplusplus /FS
  )
endif()
