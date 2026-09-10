cmake_minimum_required(VERSION 3.24)

project(ht2mp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HT2MP_PACKAGE_VERSION "0.1.0-alpha.1" CACHE STRING
    "Version used in HT2MP release package file names")
if(NOT HT2MP_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-.][0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR
    "HT2MP_PACKAGE_VERSION must be a SemVer-compatible version without a leading v")
endif()

option(HT2MP_BUILD_TESTS "Build HT2MP tests" ON)
option(HT2MP_FETCH_DEPENDENCIES "Fetch pinned third-party dependencies" ON)
option(HT2MP_SERVER_ONLY "Build only the portable coordinator and server test tools" OFF)

if(WIN32 AND NOT HT2MP_SERVER_ONLY)
  set(HT2MP_PROFILE_TEMPLATE
      "${CMAKE_CURRENT_SOURCE_DIR}/apps/launcher/assets/HT2MP.pl1")
  set(HT2MP_PROFILE_TEMPLATE_SHA256
      "F25D4DC627B366C50CA30615A0DF46B43FA4E4B20D94DEA7658199A72A8DBA34")
  if(NOT EXISTS "${HT2MP_PROFILE_TEMPLATE}")
    message(FATAL_ERROR "The canonical launcher profile template is missing")
  endif()
  file(SHA256 "${HT2MP_PROFILE_TEMPLATE}" ht2mp_profile_template_actual_sha256)
  string(TOUPPER "${ht2mp_profile_template_actual_sha256}"
    ht2mp_profile_template_actual_sha256)
  if(NOT ht2mp_profile_template_actual_sha256 STREQUAL
         HT2MP_PROFILE_TEMPLATE_SHA256)
    message(FATAL_ERROR
      "The launcher profile template hash does not match the reviewed asset")
  endif()
endif()

if(HT2MP_SERVER_ONLY)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build server dependencies statically" FORCE)
endif()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

if(HT2MP_FETCH_DEPENDENCIES)
  # ENet 1.3.18 and MinHook 1.3.4. Commit hashes are used instead of mutable
  # branch names so a rebuild cannot silently change the injected code.
  FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(enet)
  # ENet is linked statically; its headers and development archive are not
  # part of either standalone runtime package.
  set_property(DIRECTORY "${enet_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
  if(WIN32)
    # Upstream ENet's CMake target adds these libraries only for MinGW even
    # though the Win32 implementation requires them with MSVC as well.
    target_link_libraries(enet PUBLIC ws2_32 winmm)
    target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
  endif()

  if(WIN32 AND NOT HT2MP_SERVER_ONLY)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      minhook
      GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
      GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
      GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(minhook)
    # MinHook is linked statically into the bridge. Do not install its SDK.
    set_property(DIRECTORY "${minhook_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
  endif()
else()
  # Offline/distribution builds may provide permissively licensed dependencies
  # through a package manager or explicit CMAKE_PREFIX_PATH.
  find_package(enet CONFIG QUIET)
  if(TARGET ENet::enet AND NOT TARGET enet)
    add_library(enet ALIAS ENet::enet)
  elseif(TARGET unofficial-enet::enet AND NOT TARGET enet)
    add_library(enet ALIAS unofficial-enet::enet)
  endif()
  if(NOT TARGET enet AND NOT TARGET enet::enet)
    find_path(HT2MP_SYSTEM_ENET_INCLUDE_DIR enet/enet.h REQUIRED)
    find_library(HT2MP_SYSTEM_ENET_LIBRARY NAMES enet REQUIRED)
    add_library(enet UNKNOWN IMPORTED GLOBAL)
    set_target_properties(enet PROPERTIES
      IMPORTED_LOCATION "${HT2MP_SYSTEM_ENET_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${HT2MP_SYSTEM_ENET_INCLUDE_DIR}"
    )
    if(WIN32)
      set_property(TARGET enet APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "ws2_32;winmm")
    endif()
  endif()

  if(WIN32 AND NOT HT2MP_SERVER_ONLY)
    find_package(minhook CONFIG QUIET)
    if(TARGET minhook::minhook AND NOT TARGET minhook)
      add_library(minhook ALIAS minhook::minhook)
    elseif(NOT TARGET minhook)
      find_path(HT2MP_SYSTEM_MINHOOK_INCLUDE_DIR MinHook.h REQUIRED)
      find_library(HT2MP_SYSTEM_MINHOOK_LIBRARY NAMES minhook libMinHook REQUIRED)
      add_library(minhook UNKNOWN IMPORTED GLOBAL)
      set_target_properties(minhook PROPERTIES
        IMPORTED_LOCATION "${HT2MP_SYSTEM_MINHOOK_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HT2MP_SYSTEM_MINHOOK_INCLUDE_DIR}"
      )
    endif()
  endif()
endif()

if(HT2MP_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

# Subsystems own their local target definitions. Conditional inclusion keeps
# profile-analysis-only builds useful while individual components are being
# brought up.
if(HT2MP_SERVER_ONLY)
  set(ht2mp_subdirs
    src/protocol
    src/net
    apps/coordinator
    apps/sim)
  if(HT2MP_BUILD_TESTS)
    list(APPEND ht2mp_subdirs tests/protocol)
  endif()
else()
  set(ht2mp_subdirs
    src/protocol
    src/net
    src/ipc
    src/game
    src/windows
    apps/coordinator
    apps/sim
    apps/client
    apps/launcher
    bridge
    apps/injector32
    tools/profile_check
    tools/runtime_observer
    tools/timeline_replay
    tests/protocol
    tests/game
    tests/ipc)
endif()

foreach(ht2mp_subdir IN LISTS ht2mp_subdirs)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ht2mp_subdir}/CMakeLists.txt")
    add_subdirectory("${ht2mp_subdir}")
  endif()
endforeach()

if(HT2MP_SERVER_ONLY)
  install(TARGETS ht2mp-coordinator RUNTIME DESTINATION bin)
  install(FILES
    README.md
    LICENSE
    THIRD_PARTY_NOTICES.md
    docs/SERVER.md
    DESTINATION share/doc/ht2mp-server)
  if(WIN32)
    install(FILES apps/coordinator/start-server.cmd DESTINATION .)
  else()
    install(PROGRAMS apps/coordinator/start-server.sh DESTINATION .)
  endif()

  set(CPACK_PACKAGE_NAME "HT2MP-server")
  set(CPACK_PACKAGE_VENDOR "HT2MP")
  set(CPACK_PACKAGE_VERSION "${HT2MP_PACKAGE_VERSION}")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist/server")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME
      "HT2MP-server-${HT2MP_PACKAGE_VERSION}-windows-x86_64")
  else()
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME
      "HT2MP-server-${HT2MP_PACKAGE_VERSION}-linux-x86_64")
  endif()
  include(CPack)
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  install(TARGETS
    ht2mp-launcher
    ht2mp-client
    ht2mp-injector32
    ht2mp-bridge32
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .)
  install(FILES
    README.md
    LICENSE
    THIRD_PARTY_NOTICES.md
    DESTINATION .)
  install(FILES
    apps/launcher/assets/HT2MP.pl1
    apps/launcher/assets/README.txt
    DESTINATION assets)

  set(CPACK_PACKAGE_NAME "HT2MP-client")
  set(CPACK_PACKAGE_VENDOR "HT2MP")
  set(CPACK_PACKAGE_VERSION "${HT2MP_PACKAGE_VERSION}")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist/client")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_FILE_NAME
    "HT2MP-client-${HT2MP_PACKAGE_VERSION}-windows-x86")
  include(CPack)
endif()
