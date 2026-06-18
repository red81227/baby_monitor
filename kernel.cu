#include <cuda_runtime.h>
#include <device_launch_parameters.h>

// BGR 轉 Grayscale Kernel
// input: 彩色影像資料 (3 channels)
// output: 灰階影像資料 (1 channel)
__global__ void bgrToGrayKernel(unsigned char* input, unsigned char* output, int width, int height) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x < width && y < height) {
        // 1. 計算輸入 (Color) 的索引：每個像素佔 3 bytes
        int colorIdx = (y * width + x) * 3;
        
        // 2. 計算輸出 (Gray) 的索引：每個像素佔 1 byte
        int grayIdx = y * width + x;

        // 3. 讀取 BGR (注意 OpenCV 順序是 B, G, R)
        unsigned char b = input[colorIdx];
        unsigned char g = input[colorIdx + 1];
        unsigned char r = input[colorIdx + 2];

        // 4. 計算灰階值 (使用浮點數運算後轉回 unsigned char)
        // 也可以使用整數運算優化，但先用浮點數最直觀
        output[grayIdx] = (unsigned char)(0.114f * b + 0.587f * g + 0.299f * r);
    }
}

// Wrapper function
void launchBGR2Gray(unsigned char* d_input, unsigned char* d_output, int width, int height) {
    dim3 blockSize(16, 16);
    dim3 gridSize((width + blockSize.x - 1) / blockSize.x, 
                  (height + blockSize.y - 1) / blockSize.y);

    bgrToGrayKernel<<<gridSize, blockSize>>>(d_input, d_output, width, height);
    cudaDeviceSynchronize();
}

// =======================================================
// Stage 2: 移動偵測 (Motion Detection)
// =======================================================

/**
 * @brief 計算兩幀灰階影像的絕對差異，並進行二值化。
 * @param d_current 當前幀（灰階）
 * @param d_previous 前一幀（灰階）
 * @param d_diff 輸出差異影像（二值化結果）
 * @param width 影像寬度
 * @param height 影像高度
 * @param threshold 差異門檻值 (e.g., 20)，大於此值視為移動
 */
__global__ void motionDetectKernel(unsigned char* d_current, unsigned char* d_previous, unsigned char* d_diff, int width, int height, unsigned char threshold) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x < width && y < height) {
        int idx = y * width + x;

        unsigned char current_pixel = d_current[idx];
        unsigned char previous_pixel = d_previous[idx];

        // 1. 計算絕對差異值 (Absolute Difference)
        unsigned char diff_val;
        if (current_pixel > previous_pixel) {
            diff_val = current_pixel - previous_pixel;
        } else {
            diff_val = previous_pixel - current_pixel;
        }

        // 2. 應用二值化門檻 (Thresholding)
        // 如果差異大於門檻，則輸出白色 (255)，否則輸出黑色 (0)
        d_diff[idx] = (diff_val > threshold) ? 255 : 0;
    }
}

// Wrapper function for motion detection
void launchMotionDetect(unsigned char* d_current, unsigned char* d_previous, unsigned char* d_diff, int width, int height, unsigned char threshold) {
    dim3 blockSize(16, 16);
    dim3 gridSize((width + blockSize.x - 1) / blockSize.x, 
                  (height + blockSize.y - 1) / blockSize.y);

    motionDetectKernel<<<gridSize, blockSize>>>(d_current, d_previous, d_diff, width, height, threshold);
    cudaDeviceSynchronize();
}