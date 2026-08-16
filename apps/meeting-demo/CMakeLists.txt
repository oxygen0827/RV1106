cmake_minimum_required(VERSION 3.15)

project(echo_mate_meeting_demo LANGUAGES CXX)

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

target_include_directories(meeting_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    ${JSONCPP_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
)

target_link_directories(meeting_demo PRIVATE
    ${JSONCPP_LIBRARY_DIRS}
    ${OPENSSL_LIBRARY_DIRS}
    ${ALSA_LIBRARY_DIRS}
)

target_link_libraries(meeting_demo PRIVATE
    ${OPENSSL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${ALSA_LIBRARIES}
    pthread
)

set_target_properties(meeting_demo PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

install(TARGETS meeting_demo DESTINATION .)
