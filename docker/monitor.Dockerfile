# BabyMonitor 映像 (設計見 openspec/changes/containerize-services/design.md D2)
#   docker build -f docker/monitor.Dockerfile .
# 建置階段編譯程式；執行階段只含程式、前端、預設規則檔與執行所需的函式庫。

# NuGet 的 ONNX Runtime 1.27.0 GPU 是 CUDA 13 建置 (需要 libcudart.so.13、cuDNN 9)
ARG CUDA_RUNTIME_IMAGE=nvidia/cuda:13.0.2-cudnn-runtime-ubuntu24.04

# ---------------------------------------------------------------
# 建置階段
# ---------------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates unzip libopencv-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# ONNX Runtime 1.27.0 GPU (CUDA 13)：NuGet 套件 (.nupkg 即 zip)，固定版本並驗證 checksum。
# 整理成 CMake 預期的 ORT_ROOT/{include,lib}；libonnxruntime.so 的 SONAME 是 libonnxruntime.so.1
ADD --checksum=sha256:b3c00b65eabf9d17ee038f49fae4f126cd28c0ccfc91deab556307497c71ee7b \
    https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.gpu.linux/1.27.0/microsoft.ml.onnxruntime.gpu.linux.1.27.0.nupkg \
    /tmp/onnxruntime.nupkg
RUN mkdir -p /tmp/ort /opt/onnxruntime/include /opt/onnxruntime/lib \
    && unzip -q /tmp/onnxruntime.nupkg 'buildTransitive/native/include/*' 'runtimes/linux-x64/native/*' -d /tmp/ort \
    && cp /tmp/ort/buildTransitive/native/include/*.h /opt/onnxruntime/include/ \
    && cp /tmp/ort/runtimes/linux-x64/native/libonnxruntime*.so /opt/onnxruntime/lib/ \
    && ln -s libonnxruntime.so /opt/onnxruntime/lib/libonnxruntime.so.1 \
    && rm -rf /tmp/ort /tmp/onnxruntime.nupkg

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/
COPY frontend/ frontend/
COPY config/ config/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DORT_ROOT=/opt/onnxruntime \
    && cmake --build build -j"$(nproc)" --target BabyMonitor bm_replay_rules frontend

# ---------------------------------------------------------------
# 執行階段
# ---------------------------------------------------------------
FROM ${CUDA_RUNTIME_IMAGE}

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libopencv-core406t64 libopencv-imgproc406t64 libopencv-imgcodecs406t64 \
        libopencv-videoio406t64 libopencv-dnn406t64 \
        libcurl4t64 curl ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/babymonitor
COPY --from=build /opt/onnxruntime/lib/ /opt/onnxruntime/lib/
COPY --from=build /src/build/BabyMonitor /src/build/bm_replay_rules ./
COPY --from=build /src/build/frontend/ frontend/
# 預設規則檔 (Compose 會以主機的 config/ 唯讀掛載覆蓋，以便熱重載)
COPY --from=build /src/config/ config/

ENV LD_LIBRARY_PATH=/opt/onnxruntime/lib

# 確認執行檔與 CUDA provider 沒有缺少的函式庫 (缺少時建置失敗，而不是執行時才發現)。
# libcuda.so.1 由主機驅動在執行時經 NVIDIA Container Toolkit 提供，建置時不檢查。
RUN missing="$(ldd ./BabyMonitor ./bm_replay_rules /opt/onnxruntime/lib/libonnxruntime_providers_cuda.so \
        | grep 'not found' | grep -v 'libcuda\.so\.1' || true)"; \
    if [ -n "$missing" ]; then echo "缺少函式庫:"; echo "$missing"; exit 1; fi

# 資料目錄 (Compose 掛載主機目錄；以 user: UID:GID 執行，檔案屬於主機使用者)
ENV BM_LOG_DIR=/var/lib/babymonitor/logs \
    BM_EVENT_DIR=/var/lib/babymonitor/events
RUN mkdir -p /var/lib/babymonitor/logs /var/lib/babymonitor/events && chmod -R 0777 /var/lib/babymonitor

EXPOSE 18080
STOPSIGNAL SIGTERM
ENTRYPOINT ["/opt/babymonitor/BabyMonitor"]
