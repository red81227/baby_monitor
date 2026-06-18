#include <opencv2/opencv.hpp>
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h> // 用於發送 Blynk 請求

// ==========================================
// 1. Blynk 設定 (請在設定完 App 後填入)
// ==========================================
// 這兩個資訊要從 Blynk Console 取得
const std::string BLYNK_AUTH_TOKEN = "owygqfcQ3bAQeZhnB9kNpXhzbGng5gYH"; 
const std::string BLYNK_EVENT_CODE = "motion_alert"; // 事件代碼 (稍後教學設定)

// 宣告 CUDA Wrapper 函式
void launchBGR2Gray(unsigned char* d_input, unsigned char* d_output, int width, int height);
void launchMotionDetect(unsigned char* d_current, unsigned char* d_previous, unsigned char* d_diff, int width, int height, unsigned char threshold); 

// ==========================================
// 2. Blynk 警報發送函式
// ==========================================
void sendBlynkAlert() {
    CURL* curl;
    CURLcode res;
    
    // Blynk HTTP API: logEvent
    std::string url = "https://blynk.cloud/external/api/logEvent?token=" + BLYNK_AUTH_TOKEN + "&code=" + BLYNK_EVENT_CODE;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // 設定不驗證 SSL (簡化開發，避免憑證錯誤)
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        // 執行請求 (Blynk 伺服器回應很快，直接阻塞執行即可，或可用 thread 分離)
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK) {
            fprintf(stderr, "Blynk request failed: %s\n", curl_easy_strerror(res));
        } else {
            std::cout << ">> Blynk Alert Sent!" << std::endl;
        }
        curl_easy_cleanup(curl);
    }
}

// ==========================================
// 3. MJPEG 串流伺服器 (讓 Blynk App 顯示影像)
// ==========================================
class MJPEGWriter {
    int sock;
    int port;
    std::atomic<bool> isRunning;
    std::thread serverThread;
    cv::Mat lastFrame; 
    std::mutex frameMutex;

public:
    MJPEGWriter(int port = 8080) : port(port), sock(0), isRunning(false) {}

    void start() {
        isRunning = true;
        serverThread = std::thread(&MJPEGWriter::listenHelper, this);
    }

    void stop() {
        isRunning = false;
        if (serverThread.joinable()) serverThread.join();
        if (sock) close(sock);
    }

    void updateFrame(const cv::Mat& frame) {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (!frame.empty()) frame.copyTo(lastFrame);
    }

private:
    void listenHelper() {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;

        int true_v = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &true_v, sizeof(int));

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) return;
        if (listen(sock, 5) < 0) return;

        std::cout << "MJPEG Server running on port " << port << std::endl;

        while (isRunning) {
            struct sockaddr_in cli_addr;
            socklen_t clilen = sizeof(cli_addr);
            int client_sock = accept(sock, (struct sockaddr *) &cli_addr, &clilen);
            if (client_sock < 0) continue;
            std::thread(&MJPEGWriter::clientHandler, this, client_sock).detach();
        }
    }

    void clientHandler(int client_sock) {
        std::string head = "HTTP/1.0 200 OK\r\n"
                           "Server: MJPEG-Streamer\r\n"
                           "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n\r\n";
        write(client_sock, head.c_str(), head.length());

        std::vector<uchar> buf;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 70}; 

        while (isRunning) {
            cv::Mat output;
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                if (lastFrame.empty()) { usleep(100000); continue; }
                lastFrame.copyTo(output);
            }
            
            cv::imencode(".jpg", output, buf, params);
            std::string boundary = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(buf.size()) + "\r\n\r\n";
            
            if (write(client_sock, boundary.c_str(), boundary.length()) < 0) break;
            if (write(client_sock, reinterpret_cast<char*>(buf.data()), buf.size()) < 0) break;
            if (write(client_sock, "\r\n", 2) < 0) break;

            usleep(40000); // 限制約 25 FPS
        }
        close(client_sock);
    }
};

// ==========================================
// 主程式
// ==========================================
int main() {
    // 檢查是否填寫 Token
    if (BLYNK_AUTH_TOKEN == "你的_BLYNK_AUTH_TOKEN") {
        std::cerr << "錯誤: 請先在程式碼中填入你的 Blynk Auth Token！" << std::endl;
        return -1;
    }

    cv::VideoCapture cap(0);
    if (!cap.isOpened()) return -1;

    int width = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int height = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    
    cv::Mat h_colorFrame; 
    cv::Mat h_grayFrame(height, width, CV_8UC1);
    cv::Mat h_diffFrame(height, width, CV_8UC1);
    
    // 初始化 CUDA
    size_t colorSize = width * height * 3 * sizeof(unsigned char);
    size_t graySize = width * height * 1 * sizeof(unsigned char);
    unsigned char *d_input, *d_output, *d_prev_output, *d_diff_output;
    cudaMalloc((void**)&d_input, colorSize);
    cudaMalloc((void**)&d_output, graySize);
    cudaMalloc((void**)&d_prev_output, graySize); 
    cudaMalloc((void**)&d_diff_output, graySize); 
    cudaMemset(d_prev_output, 0, graySize); 

    // 啟動 MJPEG Server
    MJPEGWriter streamer(8080);
    streamer.start();

    // 警報控制
    auto lastAlertTime = std::chrono::steady_clock::now();
    bool firstRun = true;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    
    std::cout << "System Running. Connect via Blynk App or Browser: http://IP:8080" << std::endl;

    while (true) {
        cap >> h_colorFrame;
        if (h_colorFrame.empty()) break;

        cudaMemcpy(d_input, h_colorFrame.data, colorSize, cudaMemcpyHostToDevice);
        launchBGR2Gray(d_input, d_output, width, height);
        launchMotionDetect(d_output, d_prev_output, d_diff_output, width, height, 25); // Threshold
        cudaMemcpy(h_diffFrame.data, d_diff_output, graySize, cudaMemcpyDeviceToHost); 
        cudaMemcpy(d_prev_output, d_output, graySize, cudaMemcpyDeviceToDevice);

        // 影像處理
        cv::dilate(h_diffFrame, h_diffFrame, kernel, cv::Point(-1, -1), 2); 
        cv::erode(h_diffFrame, h_diffFrame, kernel); 

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(h_diffFrame, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        bool motion_detected = false;
        for (const auto& contour : contours) {
            if (cv::contourArea(contour) > 800.0) { // 稍微調高靈敏度門檻
                cv::Rect bbox = cv::boundingRect(contour);
                cv::rectangle(h_colorFrame, bbox, cv::Scalar(0, 0, 255), 2); 
                motion_detected = true;
            }
        }

        // 觸發警報
        if (motion_detected) {
            auto now = std::chrono::steady_clock::now();
            // 冷卻時間 15 秒，避免手機一直響
            if (firstRun || std::chrono::duration_cast<std::chrono::seconds>(now - lastAlertTime).count() > 15) {
                
                // 異步發送 Blynk 請求 (不卡住影像)
                std::thread(sendBlynkAlert).detach();
                
                lastAlertTime = now;
                firstRun = false;
            }
            cv::putText(h_colorFrame, "ALERT SENT", cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 0, 255), 2);
        }

        // 更新串流
        streamer.updateFrame(h_colorFrame);

        if (cv::waitKey(1) == 'q') break;
    }

    streamer.stop();
    cudaFree(d_input); cudaFree(d_output); cudaFree(d_prev_output); cudaFree(d_diff_output);
    cap.release();
    return 0;
}