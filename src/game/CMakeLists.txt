add_library(ht2mp_game STATIC
  pe_image.cpp
  pattern.cpp
  profiles.cpp
  observer.cpp
  ai_player_pool.cpp
  local_world_observer.cpp
)

target_include_directories(ht2mp_game
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(ht2mp_game PUBLIC cxx_std_20)
target_link_libraries(ht2mp_game PUBLIC ht2mp_protocol)
set_target_properties(ht2mp_game PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(ht2mp_game PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
  target_compile_definitions(ht2mp_game PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  target_compile_options(ht2mp_game PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
