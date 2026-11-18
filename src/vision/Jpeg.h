#pragma once

#include <vector>

// 部分 UVC 攝影機輸出的 MJPG 影格省略 Huffman table (DHT)，瀏覽器與 libjpeg 無法解碼。
// 若 SOS 之前沒有 DHT，就在 SOS 前插入 JPEG 標準 (ITU T.81 K.3) 的 Huffman table；
// 不解碼、不重新編碼。回傳是否有插入。不是 JPEG (不以 FFD8 開頭) 時不修改並回傳 false。
bool ensureHuffmanTables(std::vector<unsigned char>& jpeg);

// 子碼流的 reduced decode 縮小倍率 (1、2、4、8)：
// 主碼流長邊 < 1280 時為 1；否則取「縮小後長邊仍 >= 640」的最大倍率。
int reducedDecodeFactor(int width, int height);
