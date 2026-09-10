if(NOT WIN32)
  return()
endif()

add_library(ht2mp_launcher_core STATIC
  config.cpp
  profile_editor.cpp
  status.cpp
)
target_include_directories(ht2mp_launcher_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(ht2mp_launcher_core PUBLIC cxx_std_20)
target_link_libraries(ht2mp_launcher_core PUBLIC
  ht2mp_client_runtime
  ht2mp_protocol
  ht2mp::windows_runtime
  ole32
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_executable(ht2mp-launcher WIN32 main.cpp)
  set_target_properties(ht2mp-launcher PROPERTIES
    OUTPUT_NAME "ht2mp-launcher"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  )
  target_compile_features(ht2mp-launcher PRIVATE cxx_std_20)
  target_link_libraries(ht2mp-launcher PRIVATE
    ht2mp_launcher_core
    comctl32
    comdlg32
    shell32
  )
  add_custom_target(ht2mp_launcher_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "${PROJECT_BINARY_DIR}/bin/$<CONFIG>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/HT2MP.pl1"
      "${PROJECT_BINARY_DIR}/bin/$<CONFIG>/assets/HT2MP.pl1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/README.txt"
      "${PROJECT_BINARY_DIR}/bin/$<CONFIG>/assets/README.txt"
    DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/HT2MP.pl1"
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/README.txt"
    VERBATIM
  )
  add_dependencies(ht2mp-launcher ht2mp_launcher_assets)
endif()

if(MSVC)
  target_compile_options(ht2mp_launcher_core PRIVATE
    /W4 /permissive- /Zc:__cplusplus /utf-8 /FS
  )
  set_property(TARGET ht2mp_launcher_core PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(TARGET ht2mp-launcher)
    target_compile_options(ht2mp-launcher PRIVATE
      /W4 /permissive- /Zc:__cplusplus /utf-8 /FS
    )
    set_property(TARGET ht2mp-launcher PROPERTY MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

if(HT2MP_BUILD_TESTS)
  add_executable(ht2mp_profile_template_tool template_tool.cpp)
  set_target_properties(ht2mp_profile_template_tool PROPERTIES
    OUTPUT_NAME "ht2mp-profile-template-tool"
  )
  target_compile_features(ht2mp_profile_template_tool PRIVATE cxx_std_20)
  target_link_libraries(ht2mp_profile_template_tool PRIVATE ole32)
  if(MSVC)
    target_compile_options(ht2mp_profile_template_tool PRIVATE
      /W4 /permissive- /Zc:__cplusplus /utf-8 /FS
    )
    set_property(TARGET ht2mp_profile_template_tool PROPERTY MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  add_executable(ht2mp_launcher_tests launcher_tests.cpp)
  target_compile_features(ht2mp_launcher_tests PRIVATE cxx_std_20)
  target_link_libraries(ht2mp_launcher_tests PRIVATE ht2mp_launcher_core)
  if(MSVC)
    target_compile_options(ht2mp_launcher_tests PRIVATE
      /W4 /permissive- /Zc:__cplusplus /utf-8 /FS
    )
    set_property(TARGET ht2mp_launcher_tests PROPERTY MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME launcher-core COMMAND ht2mp_launcher_tests)
  add_test(NAME launcher-bundled-template COMMAND ht2mp_launcher_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/HT2MP.pl1")
  if(TARGET ht2mp-launcher)
    add_executable(ht2mp_launcher_ui_tests launcher_ui_tests.cpp)
    target_compile_features(ht2mp_launcher_ui_tests PRIVATE cxx_std_20)
    add_dependencies(ht2mp_launcher_ui_tests ht2mp-launcher)
    if(MSVC)
      target_compile_options(ht2mp_launcher_ui_tests PRIVATE
        /W4 /permissive- /Zc:__cplusplus /utf-8 /FS
      )
      set_property(TARGET ht2mp_launcher_ui_tests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME launcher-win32-ui COMMAND ht2mp_launcher_ui_tests
      "$<TARGET_FILE:ht2mp-launcher>")
  endif()
endif()
