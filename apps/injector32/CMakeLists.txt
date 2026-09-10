if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(STATUS "Skipping ht2mp-injector32: configure a Win32 build to create it")
  return()
endif()

add_library(ht2mp_injector_core STATIC injector.cpp)
target_compile_features(ht2mp_injector_core PUBLIC cxx_std_20)
target_include_directories(ht2mp_injector_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ht2mp_injector_core PUBLIC
  ht2mp_game
  ht2mp::ipc
  ht2mp::windows_runtime
)

add_executable(ht2mp-injector32 main.cpp)
set_target_properties(ht2mp-injector32 PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
target_compile_features(ht2mp-injector32 PRIVATE cxx_std_20)
target_link_libraries(ht2mp-injector32 PRIVATE ht2mp_injector_core)

if(MSVC)
  target_compile_options(ht2mp_injector_core PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
  target_compile_options(ht2mp-injector32 PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
  set_property(TARGET ht2mp_injector_core PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET ht2mp-injector32 PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(HT2MP_BUILD_TESTS)
  add_executable(ht2mp-injector-tests injector_tests.cpp)
  target_compile_features(ht2mp-injector-tests PRIVATE cxx_std_20)
  target_link_libraries(ht2mp-injector-tests PRIVATE ht2mp_injector_core)
  add_dependencies(ht2mp-injector-tests ht2mp-bridge32)
  if(MSVC)
    target_compile_options(ht2mp-injector-tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /FS)
    set_property(TARGET ht2mp-injector-tests PROPERTY MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME injector-preflight
    COMMAND ht2mp-injector-tests "$<TARGET_FILE:ht2mp-bridge32>")
endif()
