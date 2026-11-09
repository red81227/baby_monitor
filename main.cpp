#include <opencv2/opencv.hpp>
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <list>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// 宣告 CUDA Wrapper 函式
void launchBGR2Gray(unsigned char* d_input, unsigned char* d_output, int width, int height);
void launchMotionDetect(unsigned char* d_current, unsigned char* d_previous, unsigned char* d_diff, int width, int height, unsigned char threshold);

// ==========================================
// 1. 執行期設定 (全部來自環境變數)
// ==========================================
struct Config {
    int streamPort = 8080;
};

// 讀取並驗證設定；失敗時輸出錯誤並回傳 false
bool loadConfig(Config& cfg) {
    if (const char* port = std::getenv("BM_STREAM_PORT")) {
        std::size_t used = 0;
        int value = 0;
        try {
            value = std::stoi(port, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used == 0 || port[used] != '\0' || value < 1 || value > 65535) {
            std::cerr << "錯誤: BM_STREAM_PORT 必須是 1-65535 的整數 (目前為 \"" << port << "\")" << std::endl;
            return false;
        }
        cfg.streamPort = value;
    }
    return true;
}

// ==========================================
// 2. 訊號處理
// ==========================================
std::atomic<bool> g_stop{false};

void onStopSignal(int) {
    g_stop.store(true);
}

void installSignalHandlers() {
    // 寫入已關閉的 socket 時不要讓整個程式結束
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa {};
    sa.sa_handler = onStopSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// ==========================================
// 3. MJPEG 串流伺服器
// ==========================================
class MJPEGWriter {
    struct Client {
        int fd;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    int sock = -1;
    int port;
    std::atomic<bool> isRunning{false};
    std::thread serverThread;
    cv::Mat lastFrame;
    std::mutex frameMutex;
    std::list<Client> clients;
    std::mutex clientsMutex;

public:
    explicit MJPEGWriter(int port = 8080) : port(port) {}

    // 綁定 port 並開始接受連線；失敗時回傳 false
    bool start() {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("MJPEG socket");
            return false;
        }

        int true_v = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &true_v, sizeof(int));

        struct sockaddr_in serv_addr {};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0 || listen(sock, 5) < 0) {
            std::cerr << "錯誤: MJPEG 伺服器無法使用 port " << port << ": " << strerror(errno) << std::endl;
            close(sock);
            sock = -1;
            return false;
        }

        std::cout << "MJPEG Server running on port " << port << std::endl;
        isRunning = true;
        serverThread = std::thread(&MJPEGWriter::acceptLoop, this);
        return true;
    }

    void stop() {
        if (!isRunning.exchange(false)) return;

        // 讓阻塞中的 accept() 返回
        shutdown(sock, SHUT_RDWR);
        if (serverThread.joinable()) serverThread.join();
        close(sock);
        sock = -1;

        // 中斷所有客戶端連線並等待其執行緒結束
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& c : clients) shutdown(c.fd, SHUT_RDWR);
        for (auto& c : clients) {
            if (c.thread.joinable()) c.thread.join();
            close(c.fd);
        }
        clients.clear();
    }

    void updateFrame(const cv::Mat& frame) {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (!frame.empty()) frame.copyTo(lastFrame);
    }

private:
    void acceptLoop() {
        while (isRunning) {
            struct sockaddr_in cli_addr;
            socklen_t clilen = sizeof(cli_addr);
            int client_sock = accept(sock, (struct sockaddr *) &cli_addr, &clilen);
            if (client_sock < 0) continue; // stop() 時 isRunning 為 false，迴圈結束

            std::lock_guard<std::mutex> lock(clientsMutex);
            reapFinishedClients();
            auto done = std::make_shared<std::atomic<bool>>(false);
            clients.push_back(Client{client_sock, std::thread(&MJPEGWriter::clientHandler, this, client_sock, done), done});
        }
    }

    // 需持有 clientsMutex
    void reapFinishedClients() {
        for (auto it = clients.begin(); it != clients.end();) {
            if (it->done->load()) {
                it->thread.join();
                close(it->fd);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    static bool sendAll(int fd, const char* data, std::size_t len) {
        while (len > 0) {
            ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            data += n;
            len -= static_cast<std::size_t>(n);
        }
        return true;
    }

    // fd 由 stop() 或 reapFinishedClients() 關閉，避免 fd 被重複使用時誤關
    void clientHandler(int client_sock, std::shared_ptr<std::atomic<bool>> done) {
        std::string head = "HTTP/1.0 200 OK\r\n"
                           "Server: MJPEG-Streamer\r\n"
                           "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n\r\n";

        std::vector<uchar> buf;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 70};

        if (sendAll(client_sock, head.c_str(), head.length())) {
            while (isRunning) {
                cv::Mat output;
                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    if (!lastFrame.empty()) lastFrame.copyTo(output);
                }
                if (output.empty()) { usleep(100000); continue; }

                cv::imencode(".jpg", output, buf, params);
                std::string boundary = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(buf.size()) + "\r\n\r\n";

                if (!sendAll(client_sock, boundary.c_str(), boundary.length())) break;
                if (!sendAll(client_sock, reinterpret_cast<char*>(buf.data()), buf.size())) break;
                if (!sendAll(client_sock, "\r\n", 2)) break;

                usleep(40000); // 限制約 25 FPS
            }
        }
        done->store(true);
    }
};

// ==========================================
// 主程式
// ==========================================
int main() {
    installSignalHandlers();

    Config cfg;
    if (!loadConfig(cfg)) return 1;

    const int cameraIndex = 0;
    cv::VideoCapture cap(cameraIndex);
    if (!cap.isOpened()) {
        std::cerr << "錯誤: 無法開啟攝影機 (index " << cameraIndex << ")" << std::endl;
        return 1;
    }

    // 啟動 MJPEG Server
    MJPEGWriter streamer(cfg.streamPort);
    if (!streamer.start()) return 1;

    // CUDA 緩衝區在第一張影格時依實際尺寸配置
    int width = 0, height = 0;
    size_t colorSize = 0, graySize = 0;
    unsigned char *d_input = nullptr, *d_output = nullptr, *d_prev_output = nullptr, *d_diff_output = nullptr;
    cv::Mat h_colorFrame;
    cv::Mat h_diffFrame;

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    int exitCode = 0;

    std::cout << "System Running. Open in browser: http://IP:" << cfg.streamPort << std::endl;

    while (!g_stop) {
        cap >> h_colorFrame;
        if (h_colorFrame.empty()) {
            if (g_stop) break;
            std::cerr << "錯誤: 攝影機中斷，無法再取得影格" << std::endl;
            exitCode = 1;
            break;
        }

        // 第一張影格：依實際尺寸配置緩衝區，並只作為比較基準
        if (d_input == nullptr) {
            if (h_colorFrame.channels() != 3 || !h_colorFrame.isContinuous()) {
                std::cerr << "錯誤: 不支援的影格格式 (需為連續的 3 通道 BGR)" << std::endl;
                exitCode = 1;
                break;
            }
            width = h_colorFrame.cols;
            height = h_colorFrame.rows;
            colorSize = static_cast<size_t>(width) * height * 3;
            graySize = static_cast<size_t>(width) * height;
            h_diffFrame.create(height, width, CV_8UC1);
            cudaMalloc((void**)&d_input, colorSize);
            cudaMalloc((void**)&d_output, graySize);
            cudaMalloc((void**)&d_prev_output, graySize);
            cudaMalloc((void**)&d_diff_output, graySize);

            cudaMemcpy(d_input, h_colorFrame.data, colorSize, cudaMemcpyHostToDevice);
            launchBGR2Gray(d_input, d_prev_output, width, height);
            streamer.updateFrame(h_colorFrame);
            continue;
        }

        // 尺寸或格式與初始化不符時停止，避免 cudaMemcpy 越界
        if (h_colorFrame.cols != width || h_colorFrame.rows != height ||
            h_colorFrame.channels() != 3 || !h_colorFrame.isContinuous()) {
            std::cerr << "錯誤: 影格尺寸與初始化不符 (預期 " << width << "x" << height << "x3，實際 "
                      << h_colorFrame.cols << "x" << h_colorFrame.rows << "x" << h_colorFrame.channels() << ")" << std::endl;
            exitCode = 1;
            break;
        }

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

        // 標示移動
        if (motion_detected) {
            cv::putText(h_colorFrame, "MOTION", cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 0, 255), 2);
        }

        // 更新串流
        streamer.updateFrame(h_colorFrame);
    }

    std::cout << "Shutting down..." << std::endl;
    streamer.stop();
    cudaFree(d_input); cudaFree(d_output); cudaFree(d_prev_output); cudaFree(d_diff_output);
    cap.release();
    return exitCode;
}
