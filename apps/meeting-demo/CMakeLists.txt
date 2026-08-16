cmake_minimum_required(VERSION 3.15)

project(echo_mate_meeting_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(OPENSSL REQUIRED openssl)

add_executable(meeting_demo
    main.cc
    util.cc
    http_client.cc
    wss_transport.cc
    audio.cc
)

add_executable(tls_probe
    tls_probe.cc
    util.cc
    http_client.cc
    wss_transport.cc
)

add_executable(deskbot_launcher_smoke
    tests/deskbot-launcher-smoke.c
)
set_target_properties(deskbot_launcher_smoke PROPERTIES
    OUTPUT_NAME "deskbot-launcher-smoke"
)
target_link_libraries(deskbot_launcher_smoke PRIVATE pthread)

foreach(tgt meeting_demo tls_probe)
target_include_directories(${tgt} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    ${JSONCPP_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
)
endforeach()

foreach(tgt meeting_demo tls_probe)
target_link_directories(${tgt} PRIVATE
    ${JSONCPP_LIBRARY_DIRS}
    ${OPENSSL_LIBRARY_DIRS}
    ${ALSA_LIBRARY_DIRS}
)
target_link_libraries(${tgt} PRIVATE
    ${OPENSSL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${ALSA_LIBRARIES}
    pthread
)
endforeach()

set_target_properties(meeting_demo PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

install(TARGETS meeting_demo tls_probe deskbot_launcher_smoke DESTINATION .)
