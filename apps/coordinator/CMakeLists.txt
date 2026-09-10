if(TARGET enet::enet)
    set(HT2MP_ENET_TARGET enet::enet)
elseif(TARGET enet)
    set(HT2MP_ENET_TARGET enet)
else()
    message(FATAL_ERROR "ht2mp-coordinator requires an existing ENet target named enet or enet::enet")
endif()

add_executable(ht2mp-coordinator
    main.cpp
    session.cpp
)

if(WIN32 AND NOT HT2MP_SERVER_ONLY)
    add_custom_target(ht2mp_coordinator_launch_files ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start-server.cmd"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/start-server.cmd"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LOCAL_SERVER.txt"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/LOCAL_SERVER.txt"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/start-server.cmd"
            "${CMAKE_CURRENT_SOURCE_DIR}/LOCAL_SERVER.txt"
        VERBATIM
    )
    add_dependencies(ht2mp-coordinator ht2mp_coordinator_launch_files)
endif()

target_include_directories(ht2mp-coordinator PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(DEFINED enet_SOURCE_DIR)
    target_include_directories(ht2mp-coordinator SYSTEM PRIVATE "${enet_SOURCE_DIR}/include")
else()
    find_path(HT2MP_ENET_INCLUDE_DIR enet/enet.h REQUIRED)
    target_include_directories(ht2mp-coordinator SYSTEM PRIVATE "${HT2MP_ENET_INCLUDE_DIR}")
endif()
target_link_libraries(ht2mp-coordinator PRIVATE ht2mp_protocol ${HT2MP_ENET_TARGET})
target_compile_features(ht2mp-coordinator PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(ht2mp-coordinator PRIVATE /W4 /permissive- /EHsc /FS)
    target_compile_definitions(ht2mp-coordinator PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET ht2mp-coordinator PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(HT2MP_BUILD_TESTS)
    add_test(NAME coordinator.rejects-conflicting-auth
        COMMAND ${CMAKE_COMMAND}
            "-DEXECUTABLE=$<TARGET_FILE:ht2mp-coordinator>"
            "-DEXPECTED=mutually exclusive"
            "-DCLI_ARGS=--profile|steam-8138acee|--public|--token|00000000000000000000000000000000"
            -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake")
    add_test(NAME coordinator.rejects-missing-token-file
        COMMAND ${CMAKE_COMMAND}
            "-DEXECUTABLE=$<TARGET_FILE:ht2mp-coordinator>"
            "-DEXPECTED=Could not open --token-file"
            "-DCLI_ARGS=--profile|steam-8138acee|--token-file|missing-ht2mp-token-file"
            -P "${PROJECT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake")
endif()
