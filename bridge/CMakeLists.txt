if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(STATUS "Skipping ht2mp-bridge32: configure a Win32 build to create it")
  return()
endif()

if(NOT TARGET minhook)
  message(FATAL_ERROR "ht2mp-bridge32 requires the pinned MinHook target")
endif()

add_library(ht2mp_bridge_runtime STATIC
  online_world_policy.cpp
  remote_actor_registry.cpp
  remote_command_queue.cpp
  remote_pose_timeline.cpp
  runtime_validation.cpp
)
target_include_directories(ht2mp_bridge_runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(ht2mp_bridge_runtime PUBLIC cxx_std_20)
target_link_libraries(ht2mp_bridge_runtime PUBLIC ht2mp_game)

add_library(ht2mp-bridge32 SHARED
  actor_diagnostics.cpp
  auto_enter.cpp
  bridge.cpp
  horn.cpp
  online_world.cpp
  remote_actor_backend.cpp
  replication.cpp
  telemetry.cpp
  ht2mp-bridge32.def
)
target_compile_features(ht2mp-bridge32 PRIVATE cxx_std_20)
target_link_libraries(ht2mp-bridge32 PRIVATE
  ht2mp::ipc
  ht2mp_bridge_runtime
  minhook
)
set_target_properties(ht2mp-bridge32 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "ht2mp-bridge32"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
)

if(MSVC)
  target_compile_options(ht2mp_bridge_runtime PRIVATE
    /W4 /permissive- /Zc:__cplusplus /FS
  )
  target_compile_definitions(ht2mp-bridge32 PRIVATE
    _HAS_EXCEPTIONS=0
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
  target_compile_options(ht2mp-bridge32 PRIVATE
    /W4 /permissive- /Zc:__cplusplus /GR- /FS
  )
  set_property(TARGET ht2mp-bridge32 PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET ht2mp_bridge_runtime PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(HT2MP_BUILD_TESTS)
  add_executable(ht2mp-bridge-runtime-tests runtime_validation_tests.cpp)
  target_compile_features(ht2mp-bridge-runtime-tests PRIVATE cxx_std_20)
  target_link_libraries(ht2mp-bridge-runtime-tests PRIVATE ht2mp_bridge_runtime)
  if(MSVC)
    target_compile_options(ht2mp-bridge-runtime-tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS
    )
  endif()
  add_test(NAME bridge-runtime-validation COMMAND ht2mp-bridge-runtime-tests)

  add_executable(ht2mp-bridge-command-queue-tests
    remote_command_queue_tests.cpp
  )
  target_compile_features(ht2mp-bridge-command-queue-tests PRIVATE cxx_std_20)
  target_link_libraries(ht2mp-bridge-command-queue-tests PRIVATE
    ht2mp_bridge_runtime
  )
  if(MSVC)
    target_compile_options(ht2mp-bridge-command-queue-tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS
    )
  endif()
  add_test(NAME bridge-command-queue
    COMMAND ht2mp-bridge-command-queue-tests
  )

  add_executable(ht2mp-bridge-pose-timeline-tests
    remote_pose_timeline_tests.cpp
  )
  target_compile_features(ht2mp-bridge-pose-timeline-tests PRIVATE cxx_std_20)
  target_link_libraries(ht2mp-bridge-pose-timeline-tests PRIVATE
    ht2mp_bridge_runtime
  )
  if(MSVC)
    target_compile_options(ht2mp-bridge-pose-timeline-tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS
    )
  endif()
  add_test(NAME bridge-pose-timeline
    COMMAND ht2mp-bridge-pose-timeline-tests
  )

  add_executable(ht2mp-bridge-actor-registry-tests
    remote_actor_registry_tests.cpp
  )
  target_compile_features(ht2mp-bridge-actor-registry-tests PRIVATE cxx_std_20)
  target_link_libraries(ht2mp-bridge-actor-registry-tests PRIVATE
    ht2mp_bridge_runtime
  )
  if(MSVC)
    target_compile_options(ht2mp-bridge-actor-registry-tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS
    )
  endif()
  add_test(NAME bridge-actor-registry
    COMMAND ht2mp-bridge-actor-registry-tests
  )

  add_executable(ht2mp-bridge-online-world-policy-tests
    online_world_policy_tests.cpp
  )
  target_compile_features(ht2mp-bridge-online-world-policy-tests PRIVATE
    cxx_std_20
  )
  target_link_libraries(ht2mp-bridge-online-world-policy-tests PRIVATE
    ht2mp_bridge_runtime
  )
  if(MSVC)
    target_compile_options(ht2mp-bridge-online-world-policy-tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS
    )
  endif()
  add_test(NAME bridge-online-world-policy
    COMMAND ht2mp-bridge-online-world-policy-tests
  )
endif()
