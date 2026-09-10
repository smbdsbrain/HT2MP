if(NOT WIN32)
  return()
endif()

add_library(ht2mp_client_runtime STATIC
  edition.cpp
  staging.cpp
)
target_include_directories(ht2mp_client_runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(ht2mp_client_runtime PUBLIC cxx_std_20)
target_link_libraries(ht2mp_client_runtime PUBLIC
  ht2mp::windows_runtime
)

add_executable(ht2mp-client
  main.cpp
  network_adapter.cpp
  pipe_server.cpp
  run.cpp
)
set_target_properties(ht2mp-client PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

target_compile_features(ht2mp-client PRIVATE cxx_std_20)
target_link_libraries(ht2mp-client PRIVATE
  ht2mp_game
  ht2mp::ipc
  ht2mp::windows_runtime
  ht2mp_net
  ht2mp_client_runtime
  advapi32
  winmm
)

if(MSVC)
  target_compile_options(ht2mp_client_runtime PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
  set_property(TARGET ht2mp_client_runtime PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(ht2mp-client PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
  set_property(TARGET ht2mp-client PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(HT2MP_BUILD_TESTS)
  add_executable(ht2mp_staging_tests
    edition.cpp
    staging.cpp
    staging_tests.cpp
  )
  target_compile_features(ht2mp_staging_tests PRIVATE cxx_std_20)
  target_compile_definitions(ht2mp_staging_tests PRIVATE HT2MP_STAGING_TESTING)
  target_link_libraries(ht2mp_staging_tests PRIVATE ht2mp::windows_runtime)
  if(MSVC)
    target_compile_options(ht2mp_staging_tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS)
    set_property(TARGET ht2mp_staging_tests PROPERTY MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME client.staging-safety COMMAND ht2mp_staging_tests)

  add_test(NAME client.online-requires-server
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:ht2mp-client>"
      "-DEXPECTED=--online-mode requires --server"
      "-DCLI_ARGS=run|--edition|steam|--online-mode"
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake")

  add_test(NAME client.online-rejects-non-steam
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:ht2mp-client>"
      "-DEXPECTED=supported only for the exact steam-8138acee profile"
      "-DCLI_ARGS=run|--edition|gog|--online-mode|--server|127.0.0.1:28020|--token|00000000000000000000000000000000"
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake")

  add_test(NAME client.rejects-invalid-instance
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:ht2mp-client>"
      "-DEXPECTED=Invalid --instance"
      "-DCLI_ARGS=stage|--edition|steam|--source|.|--instance|Player_A"
      -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake")
endif()
