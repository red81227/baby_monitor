#include "check.h"
#include "vision/ActivityScorer.h"

#include <opencv2/imgproc.hpp>

int main() {
    // 640x360 的 BGR 子碼流 (縮小後為 160x90)
    cv::Mat base(360, 640, CV_8UC3, cv::Scalar(90, 100, 110));
    cv::randu(base, cv::Scalar::all(80), cv::Scalar::all(120));  // 輕微雜訊，模擬真實畫面

    // 第一張影格分數為 0
    ActivityScorer scorer;
    CHECK(scorer.score(base) == 0.0);

    // 相同影像分數為 0
    CHECK(scorer.score(base) == 0.0);
    CHECK(scorer.score(base.clone()) == 0.0);

    // 局部變化：一塊 160x90 (原圖 1/16 面積) 的區域由暗變亮 → 約 6.25%
    cv::Mat changed = base.clone();
    cv::rectangle(changed, cv::Rect(0, 0, 160, 90), cv::Scalar(255, 255, 255), cv::FILLED);
    const double s = scorer.score(changed);
    CHECK(s > 0.05 && s < 0.075);
    CHECK(s > ActivityScorer::kActiveThreshold);

    // 變化後靜止：回到 0
    CHECK(scorer.score(changed) == 0.0);

    // 小於門檻的亮度飄移 (每個像素 +10，低於 25) 不算變化
    cv::Mat brighter = changed + cv::Scalar::all(10);
    CHECK(scorer.score(brighter) < ActivityScorer::kActiveThreshold);

    // 整張畫面改變：接近 1
    cv::Mat inverted = cv::Scalar::all(255) - brighter;
    CHECK(scorer.score(inverted) > 0.9);

    // 灰階輸入也可以；尺寸改變時重新開始 (分數 0)
    cv::Mat gray(480, 640, CV_8UC1, cv::Scalar(50));
    CHECK(scorer.score(gray) == 0.0);
    CHECK(scorer.score(gray) == 0.0);

    // 空影像
    CHECK(scorer.score(cv::Mat()) == 0.0);

    // ROI：只計算區域內的變化
    {
        ActivityScorer roiScorer;
        const cv::Rect2f rightHalf(0.5f, 0.0f, 0.5f, 1.0f);
        CHECK(roiScorer.score(base, rightHalf) == 0.0);  // 第一張
        // 左半邊改變：區域外，分數 0
        cv::Mat leftChanged = base.clone();
        cv::rectangle(leftChanged, cv::Rect(0, 0, 320, 360), cv::Scalar(255, 255, 255), cv::FILLED);
        CHECK(roiScorer.score(leftChanged, rightHalf) == 0.0);
        // 右半邊一半面積改變：約 50%
        cv::Mat rightChanged = leftChanged.clone();
        cv::rectangle(rightChanged, cv::Rect(320, 0, 160, 360), cv::Scalar(0, 0, 0), cv::FILLED);
        const double r = roiScorer.score(rightChanged, rightHalf);
        CHECK(r > 0.45 && r < 0.55);
        // 區域改變時重新建立基準
        CHECK(roiScorer.score(base, cv::Rect2f(0, 0, 0.5f, 1.0f)) == 0.0);
        // 區域完全在畫面外
        CHECK(roiScorer.score(base, cv::Rect2f(1.5f, 1.5f, 0.2f, 0.2f)) == 0.0);
    }

    return 0;
}
