cmake_minimum_required(VERSION 3.15)

project(echo_mate_camera_yolo LANGUAGES C CXX)

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

add_executable(camera_yolo
    main.cc
    "${YOLO_ROOT}/postprocess.cc"
    "${YOLO_ROOT}/rknpu2/yolov5_rv1106_1103.cc"
)

target_include_directories(camera_yolo PRIVATE
    "${YOLO_ROOT}"
    "${YOLO_ROOT}/utils"
    "${YOLO_ROOT}/3rdparty/rknpu2/include"
)

target_link_libraries(camera_yolo PRIVATE
    imageutils
    fileutils
    imagedrawing
    "${LIBRKNNRT}"
    dl
    m
    pthread
)

set_target_properties(camera_yolo PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib"
)

install(TARGETS camera_yolo DESTINATION .)
install(FILES
    "${YOLO_ROOT}/../model/yolov5.rknn"
    "${YOLO_ROOT}/../model/coco_80_labels_list.txt"
    DESTINATION model
)
