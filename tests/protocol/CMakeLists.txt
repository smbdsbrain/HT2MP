function(ht2mp_add_protocol_test target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE ht2mp_protocol)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /FS)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

ht2mp_add_protocol_test(ht2mp_codec_tests codec_tests.cpp)
ht2mp_add_protocol_test(ht2mp_validation_tests validation_tests.cpp)
ht2mp_add_protocol_test(ht2mp_interpolation_tests interpolation_tests.cpp)

if(TARGET ht2mp_sim_protocol)
    add_executable(ht2mp_sim_server_protocol_tests sim_server_protocol_tests.cpp)
    target_include_directories(ht2mp_sim_server_protocol_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/apps/sim"
    )
    target_link_libraries(ht2mp_sim_server_protocol_tests PRIVATE ht2mp_sim_protocol)
    target_compile_features(ht2mp_sim_server_protocol_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ht2mp_sim_server_protocol_tests PRIVATE /W4 /permissive- /EHsc /FS)
        target_compile_definitions(ht2mp_sim_server_protocol_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        set_property(TARGET ht2mp_sim_server_protocol_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ht2mp_sim_server_protocol_tests COMMAND ht2mp_sim_server_protocol_tests)
endif()

add_executable(ht2mp_session_tests
    session_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/coordinator/session.cpp"
)
target_include_directories(ht2mp_session_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/apps/coordinator"
)
target_link_libraries(ht2mp_session_tests PRIVATE ht2mp_protocol)
target_compile_features(ht2mp_session_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ht2mp_session_tests PRIVATE /W4 /permissive- /EHsc /FS)
    target_compile_definitions(ht2mp_session_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET ht2mp_session_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_test(NAME ht2mp_session_tests COMMAND ht2mp_session_tests)

ht2mp_add_protocol_test(ht2mp_replication_tests replication_tests.cpp)
target_sources(ht2mp_replication_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/coordinator/session.cpp")
target_include_directories(ht2mp_replication_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/coordinator")
if(TARGET ht2mp_ipc)
    target_link_libraries(ht2mp_replication_tests PRIVATE ht2mp_ipc)
    target_compile_definitions(ht2mp_replication_tests PRIVATE HT2MP_TEST_IPC=1)
endif()

if(TARGET ht2mp_net AND (TARGET enet OR TARGET enet::enet))
    if(TARGET enet::enet)
        set(HT2MP_TEST_ENET_TARGET enet::enet)
    else()
        set(HT2MP_TEST_ENET_TARGET enet)
    endif()
    add_executable(ht2mp_transport_tests transport_tests.cpp)
    target_include_directories(ht2mp_transport_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED enet_SOURCE_DIR)
        target_include_directories(ht2mp_transport_tests SYSTEM PRIVATE "${enet_SOURCE_DIR}/include")
    else()
        find_path(HT2MP_ENET_INCLUDE_DIR enet/enet.h REQUIRED)
        target_include_directories(ht2mp_transport_tests SYSTEM PRIVATE "${HT2MP_ENET_INCLUDE_DIR}")
    endif()
    target_link_libraries(ht2mp_transport_tests PRIVATE ht2mp_net ${HT2MP_TEST_ENET_TARGET})
    target_compile_features(ht2mp_transport_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ht2mp_transport_tests PRIVATE /W4 /permissive- /EHsc /FS)
        target_compile_definitions(ht2mp_transport_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        set_property(TARGET ht2mp_transport_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ht2mp_transport_tests COMMAND ht2mp_transport_tests)

    if(WIN32 AND TARGET ht2mp_ipc AND TARGET ht2mp_windows_runtime)
        add_executable(ht2mp_network_adapter_tests
            network_adapter_tests.cpp
            "${PROJECT_SOURCE_DIR}/apps/client/network_adapter.cpp"
            "${PROJECT_SOURCE_DIR}/apps/client/pipe_server.cpp"
        )
        target_include_directories(ht2mp_network_adapter_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/apps/client"
        )
        if(DEFINED enet_SOURCE_DIR)
            target_include_directories(ht2mp_network_adapter_tests SYSTEM PRIVATE
                "${enet_SOURCE_DIR}/include")
        else()
            find_path(HT2MP_ENET_INCLUDE_DIR enet/enet.h REQUIRED)
            target_include_directories(ht2mp_network_adapter_tests SYSTEM PRIVATE
                "${HT2MP_ENET_INCLUDE_DIR}")
        endif()
        target_link_libraries(ht2mp_network_adapter_tests PRIVATE
            ht2mp_ipc
            ht2mp_windows_runtime
            ht2mp_net
            ${HT2MP_TEST_ENET_TARGET}
            advapi32
        )
        target_compile_features(ht2mp_network_adapter_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(ht2mp_network_adapter_tests PRIVATE
                /W4 /permissive- /EHsc /FS)
            target_compile_definitions(ht2mp_network_adapter_tests PRIVATE
                NOMINMAX WIN32_LEAN_AND_MEAN)
            set_property(TARGET ht2mp_network_adapter_tests PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME ht2mp_network_adapter_tests
                 COMMAND ht2mp_network_adapter_tests)
    endif()
endif()
