cmake_minimum_required(VERSION 3.20)

project(BabyMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------
# ONNX Runtime (GPU 發行包，不進 git)
# ---------------------------------------------------------------
set(ORT_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime" CACHE PATH "ONNX Runtime 1.27 GPU 發行包的解壓目錄")
if(NOT EXISTS "${ORT_ROOT}/include/onnxruntime_cxx_api.h" OR NOT EXISTS "${ORT_ROOT}/lib/libonnxruntime.so")
    message(FATAL_ERROR
        "找不到 ONNX Runtime: ${ORT_ROOT}\n"
        "請下載 onnxruntime-linux-x64-gpu_cuda12-1.27.0.tgz\n"
        "(https://github.com/microsoft/onnxruntime/releases/tag/v1.27.0)\n"
        "解壓到上述路徑，或以 -DORT_ROOT=<路徑> 指定。")
endif()

add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ORT_ROOT}/lib/libonnxruntime.so"
    INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include")

# ---------------------------------------------------------------
# Crow (header-only HTTP) + standalone asio
# ---------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2)
FetchContent_Declare(crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.1)
# 只需要標頭檔，不執行它們的 CMakeLists
foreach(dep asio crow)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})
    endif()
endforeach()

add_library(crow_headers INTERFACE)
target_include_directories(crow_headers SYSTEM INTERFACE
    "${asio_SOURCE_DIR}/asio/include"
    "${crow_SOURCE_DIR}/include")
target_compile_definitions(crow_headers INTERFACE ASIO_STANDALONE)
target_link_libraries(crow_headers INTERFACE Threads::Threads)

# ---------------------------------------------------------------
# 核心程式庫 (主程式與測試共用)
# ---------------------------------------------------------------
set(BM_WARNINGS -Wall -Wextra)

add_library(bm_core STATIC
    src/config/Config.cpp
    src/models/MonitorState.cpp
    src/vision/YoloDecoder.cpp
    src/vision/MotionTracker.cpp
    src/vision/YoloDetector.cpp
    src/vision/VisionPipeline.cpp)
target_include_directories(bm_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(bm_core SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(bm_core PUBLIC ${OpenCV_LIBS} onnxruntime Threads::Threads)
target_compile_options(bm_core PRIVATE ${BM_WARNINGS})

add_executable(BabyMonitor src/main.cpp)
target_link_libraries(BabyMonitor PRIVATE bm_core crow_headers)
target_compile_options(BabyMonitor PRIVATE ${BM_WARNINGS})
set_target_properties(BabyMonitor PROPERTIES BUILD_RPATH "${ORT_ROOT}/lib")

# 前端檔案放到執行檔旁邊 (StaticController 以執行檔目錄為根)
add_custom_command(TARGET BabyMonitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/frontend"
            "$<TARGET_FILE_DIR:BabyMonitor>/frontend")

# ---------------------------------------------------------------
# 測試
# ---------------------------------------------------------------
enable_testing()

foreach(name config monitor_state yolo_decoder motion_tracker yolo_inference)
    add_executable(test_${name} tests/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE bm_core)
    target_compile_options(test_${name} PRIVATE ${BM_WARNINGS})
    set_target_properties(test_${name} PROPERTIES BUILD_RPATH "${ORT_ROOT}/lib")
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# 沒有模型時回傳 77 => CTest 標記為 skipped
set_tests_properties(yolo_inference PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "BM_MODEL_PATH=${CMAKE_SOURCE_DIR}/models/yolo26n.onnx;BM_TEST_IMAGE=${CMAKE_SOURCE_DIR}/tests/data/bus.jpg")
