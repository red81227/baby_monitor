# Tasks

## 1. 設定與模型

- [x] 1.1 `Config` 的 `BM_MODEL_PATH` 預設改為 `models/yolo26n-pose.onnx`，更新 `tests/test_config.cpp` 的預設值檢查；README 的環境變數表與模型匯出步驟改為 `yolo export model=yolo26n-pose.pt format=onnx imgsz=640 end2end=True`，說明不加 `end2end=True` 會得到不支援的 `[1, 56, 8400]`，並寫明舊部署的升級方式（匯出新模型或設定 `BM_MODEL_PATH`）；`ctest -R config` 通過

## 2. 解碼與偵測

- [x] 2.1 `Detection` 加上 `std::optional<std::array<Keypoint, 17>> keypoints`；`decodeBestDetection` 增加每列欄數參數（6 或 57），57 欄時以與框相同的換算與 clamp 解出關鍵點（D2）；在 `tests/test_yolo_decoder.cpp` 以手造的 57 欄資料驗證：關鍵點座標換算（含 `reduceFactor` 等效的 scale）、超出影格的點被 clamp、可見度原樣保留、6 欄時 `keypoints` 為空，`ctest -R yolo_decoder` 通過
- [x] 2.2 `YoloDetector` 載入時依輸出欄數辨識 pose / 純偵測，其他形狀拋出含模型路徑與形狀的錯誤，啟動訊息顯示模型類型（D1）；CMake 測試環境改指向 pose 模型，`tests/test_yolo_inference.cpp` 以 `bus.jpg` 驗證 pose 模型回報 17 個落在影像範圍內、可見度高者大多在框內的關鍵點，半解析度推論的關鍵點與全解析度相差不超過數個像素，且 `models/yolo26n.onnx` 存在時純偵測模型仍偵測到人、`keypoints` 為空；`ctest -R yolo_inference` 通過
- [x] 2.3 在 `test_yolo_inference` 加入推論時間量測（暖機後 50 次，輸出平均與最大毫秒數、執行裝置、模型類型，不設門檻，D5），把 CUDA 下 pose 與純偵測模型的實測數字寫進 design.md 的「實測結果」段落
- [x] 2.4 以 `[1, 56, 8400]` 的模型（未加 `end2end=True` 的匯出）實際啟動 BabyMonitor，確認輸出包含模型路徑與形狀的錯誤訊息且結束碼不為 0；以 pose 模型啟動確認啟動訊息顯示 `pose`

## 3. 即時影像 metadata

- [x] 3.1 `live::encodeDetection` 改用 `std::string` 組字串並加上 `keypoints`（17 組 `[x, y, v]` 或 `null`，D3），既有欄位不變；`tests/test_live_protocol.cpp` 驗證有 / 沒有關鍵點兩種輸出都是合法 JSON、既有欄位內容不變、關鍵點數量與數值正確，`ctest -R live_protocol` 通過
- [x] 3.2 README 的「即時影像」訊息格式補上 `keypoints` 欄位說明（順序為 COCO 17 點、座標單位、可見度範圍、沒有時為 `null`）；以腳本化 WebSocket 客戶端連到實機服務，確認收到的 detection 訊息有 17 個關鍵點且座標都在主碼流寬高範圍內

## 4. 前端骨架

- [x] 4.1 `frontend/app.js` 的 `render()` 在畫框後依 D4 畫骨架（可見度門檻 0.5、兩端都達門檻才畫線、顏色同框、尺寸依 canvas 寬度縮放），沒有 `keypoints` 時只畫框；重新 build 後確認執行檔旁的 `frontend/` 與原始碼一致，並在瀏覽器手動確認：站在鏡頭前骨架對齊身體、用東西遮住下半身時不畫下半身、以 `BM_MODEL_PATH` 切回純偵測模型時只畫框且 console 沒有錯誤

## 5. 整合驗證

- [x] 5.1 以 pose 模型在實機執行 10 分鐘：`/api/status` 的 `seq` 持續增加、`detectHz` 依動靜變化（行為與 phase1 相同），`ctest` 全部通過
- [x] 5.2 執行 `openspec validate pose-keypoints --strict` 並修正所有問題

## Workflow follow-up

- `vision-pipeline-phase1` 歸檔之後才能歸檔本變更（`live-view` capability 由它新增）
- 依 2.3 的推論時間決定是否開 TensorRT 的 change；之後另開「以關鍵點判斷移動」的 change
