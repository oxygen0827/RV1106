cmake_minimum_required(VERSION 3.15)

project(echo_mate_yolo_smoke LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(TARGET_SOC rv1106)
set(YOLO_ROOT
    "/workspace/Echo-Mate/Demo/yolov5_demo/cpp"
    CACHE PATH "Path to the Echo-Mate YOLOv5 C++ source")

if(NOT EXISTS "${YOLO_ROOT}/rknpu2/yolov5_rv1106_1103.cc")
    message(FATAL_ERROR "Echo-Mate YOLO source not found under ${YOLO_ROOT}")
endif()

add_definitions(-DRV1106_1103)
add_subdirectory("${YOLO_ROOT}/3rdparty" 3rdparty.out)
add_subdirectory("${YOLO_ROOT}/utils" utils.out)

add_executable(yolo_smoke
    main.cc
    "${YOLO_ROOT}/postprocess.cc"
    "${YOLO_ROOT}/rknpu2/yolov5_rv1106_1103.cc"
)

target_include_directories(yolo_smoke PRIVATE
    "${YOLO_ROOT}"
    "${YOLO_ROOT}/utils"
    "${YOLO_ROOT}/3rdparty/rknpu2/include"
)

target_link_libraries(yolo_smoke PRIVATE
    imageutils
    fileutils
    imagedrawing
    "${LIBRKNNRT}"
    dl
    m
    pthread
)

set_target_properties(yolo_smoke PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib"
)

install(TARGETS yolo_smoke DESTINATION .)
install(FILES
    "${YOLO_ROOT}/../model/yolov5.rknn"
    "${YOLO_ROOT}/../model/coco_80_labels_list.txt"
    "${YOLO_ROOT}/../model/bus.jpg"
    DESTINATION model
)
