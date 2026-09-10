if(NOT HT2MP_BUILD_TESTS)
  return()
endif()

add_executable(ht2mp_game_tests game_tests.cpp)
target_link_libraries(ht2mp_game_tests PRIVATE ht2mp_game)
target_compile_features(ht2mp_game_tests PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(ht2mp_game_tests PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
  target_compile_definitions(ht2mp_game_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ht2mp_game_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_test(NAME game.profiles COMMAND ht2mp_game_tests)
