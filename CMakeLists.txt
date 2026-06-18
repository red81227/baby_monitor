cmake_minimum_required(VERSION 3.10)

# 處理 CUDA 路徑
if(NOT DEFINED CUDAToolkit_ROOT)
    set(CUDAToolkit_ROOT "/usr/local/cuda")
endif()

project(BabyMonitorCUDA LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_EXTENSIONS OFF)

# 尋找套件
find_package(OpenCV REQUIRED)
find_package(CURL REQUIRED)      # 新增: 用於發送 Blynk 請求
find_package(Threads REQUIRED)   # 新增: 用於 MJPEG 伺服器多執行緒

include_directories(${OpenCV_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})

# 設定 CUDA
set(CUDA_SEPARABLE_COMPILATION ON)

# 原始碼
set(SOURCE_FILES main.cpp kernel.cu)

add_executable(BabyMonitor ${SOURCE_FILES})

# 連結函式庫 (加入 CURL 和 Threads)
target_link_libraries(BabyMonitor ${OpenCV_LIBS} ${CURL_LIBRARIES} Threads::Threads)

set_target_properties(BabyMonitor PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "native" 
)

message(STATUS "準備編譯 BabyMonitor (Blynk IoT 版本)...")