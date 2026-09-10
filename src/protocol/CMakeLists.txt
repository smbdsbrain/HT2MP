add_library(ht2mp_protocol STATIC
    codec.cpp
    interpolation.cpp
    validation.cpp
    vehicle_catalog.cpp
)

target_include_directories(ht2mp_protocol
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(ht2mp_protocol PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ht2mp_protocol PRIVATE /W4 /permissive- /EHsc /FS)
    set_property(TARGET ht2mp_protocol PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(ht2mp_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()
