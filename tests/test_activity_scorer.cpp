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

    return 0;
}
