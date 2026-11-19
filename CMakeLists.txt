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
# yaml-cpp (狀況規則檔) + libcurl (呼叫本機 VLM 服務)
# ---------------------------------------------------------------
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

find_package(CURL REQUIRED)

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
    src/vision/Jpeg.cpp
    src/vision/FrameSource.cpp
    src/vision/CaptureThread.cpp
    src/vision/ActivityScorer.cpp
    src/vision/DetectScheduler.cpp
    src/vision/AnalysisThread.cpp
    src/vision/LiveProtocol.cpp
    src/common/SystemdNotifier.cpp
    src/common/StallWatchdog.cpp
    src/situation/SignalExtractor.cpp
    src/situation/Rules.cpp
    src/situation/RuleEngine.cpp
    src/situation/RulesStore.cpp
    src/situation/ObservationLog.cpp
    src/situation/SituationMonitor.cpp
    src/situation/Replay.cpp
    src/events/EventRecorder.cpp
    src/judge/VlmJudge.cpp
    src/judge/PatrolScheduler.cpp
    src/alerts/AlertManager.cpp
    src/roi/RoiService.cpp
    src/common/Sha256.cpp)
target_include_directories(bm_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(bm_core SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(bm_core PUBLIC ${OpenCV_LIBS} onnxruntime Threads::Threads yaml-cpp::yaml-cpp CURL::libcurl crow_headers)
target_compile_options(bm_core PRIVATE ${BM_WARNINGS})

add_executable(BabyMonitor src/main.cpp)
target_link_libraries(BabyMonitor PRIVATE bm_core crow_headers)
target_compile_options(BabyMonitor PRIVATE ${BM_WARNINGS})
set_target_properties(BabyMonitor PROPERTIES BUILD_RPATH "${ORT_ROOT}/lib")

# 離線重播工具：以規則檔重新判斷觀測紀錄
add_executable(bm_replay_rules src/tools/bm_replay_rules.cpp)
target_link_libraries(bm_replay_rules PRIVATE bm_core)
target_compile_options(bm_replay_rules PRIVATE ${BM_WARNINGS})
set_target_properties(bm_replay_rules PROPERTIES BUILD_RPATH "${ORT_ROOT}/lib")

# 前端檔案放到執行檔旁邊 (StaticController 以執行檔目錄為根)
# 用獨立 target 而非 POST_BUILD：只改前端、執行檔沒重新連結時也要同步
add_custom_target(frontend ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/frontend"
            "$<TARGET_FILE_DIR:BabyMonitor>/frontend"
    COMMENT "Syncing frontend/")
add_dependencies(frontend BabyMonitor)

# 預設狀況規則與 prompt 放到執行檔旁 (從 build/ 啟動時 BM_RULES_PATH 的預設值可直接使用)
add_custom_target(situation_config ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:BabyMonitor>/config"
    COMMENT "Syncing config/")
add_dependencies(situation_config BabyMonitor)

# ---------------------------------------------------------------
# 測試
# ---------------------------------------------------------------
enable_testing()

foreach(name config monitor_state yolo_decoder motion_tracker yolo_inference latest_slot jpeg capture_thread activity_scorer detect_scheduler live_protocol systemd_notifier stall_watchdog signal_extractor rules rule_engine rules_store observation_log replay_rules ring_buffer event_recorder vlm_judge alert_manager patrol_scheduler roi_service)
    add_executable(test_${name} tests/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE bm_core)
    target_compile_options(test_${name} PRIVATE ${BM_WARNINGS})
    set_target_properties(test_${name} PROPERTIES BUILD_RPATH "${ORT_ROOT}/lib")
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# live_protocol 用 Crow 的 JSON parser 驗證輸出是合法 JSON
target_link_libraries(test_live_protocol PRIVATE crow_headers)

# rules 測試也會載入 repo 隨附的預設規則檔
target_compile_definitions(test_rules PRIVATE BM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# 重播測試使用固定的資料與 UTC 時區 (輸出含當地時間)
target_compile_definitions(test_replay_rules PRIVATE BM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_tests_properties(replay_rules PROPERTIES ENVIRONMENT "TZ=UTC")
add_test(NAME replay_rules_cli
    COMMAND bm_replay_rules "${CMAKE_SOURCE_DIR}/tests/data/replay/rules.yaml"
            "${CMAKE_SOURCE_DIR}/tests/data/replay/observations.jsonl")
set_tests_properties(replay_rules_cli PROPERTIES ENVIRONMENT "TZ=UTC"
    PASS_REGULAR_EXPRESSION "共 4 次觸發")

# 沒有模型時回傳 77 => CTest 標記為 skipped
set_tests_properties(yolo_inference PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "BM_MODEL_PATH=${CMAKE_SOURCE_DIR}/models/yolo26n-pose.onnx;BM_DETECT_MODEL_PATH=${CMAKE_SOURCE_DIR}/models/yolo26n.onnx;BM_TEST_IMAGE=${CMAKE_SOURCE_DIR}/tests/data/bus.jpg")
