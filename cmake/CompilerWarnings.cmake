function(ht2mp_enable_warnings target)
  if(MSVC)
    # Multiple CMake targets are intentionally built in parallel. /FS makes
    # MSVC serialize access to a shared program database instead of failing
    # nondeterministically with C1041.
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /FS)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()
