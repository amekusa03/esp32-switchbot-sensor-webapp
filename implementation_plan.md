# B3950 10k NTC サーミスタ温度計測機能の追加計画 (GPIO3)

ESP32-C3 の **GPIO3 (ADC1_CH3)** を利用して、B3950 10k NTC サーミスタと 10kΩ 固定抵抗による温度計測機能をシステムに追加します。

---

## 🔌 ハードウェア構成

- **接続ピン**: `GPIO3` (`ADC1_CHANNEL_3`)
  - ※ GPIO2はストラップピンのため起動トラブルを回避して **GPIO3** を採用します。
- **回路**:
  - `3.3V` ━━━ `[ B3950 サーミスタ ]` ━━━ `GPIO3 (ADC1_CH3)` ━━━ `[ 10kΩ 固定抵抗 ]` ━━━ `GND`

---

## User Review Required

> [!IMPORTANT]
> 1. **配線ピンの確定**: 使用ピンは **GPIO3** (ADC1_CHANNEL_3) となります。
> 2. **固定抵抗の精度**: サーミスタの対となるプルダウン固定抵抗は **10kΩ (±1%推奨)** をご用意ください。
> 3. **Matter エンドポイントの拡張 (任意)**: 今回はWeb UIおよびREST APIでの温度表示を主要機能としますが、必要に応じて Matter Over Wi-Fi の Temperature Sensor エンドポイント (`0x0302`) 追加にも拡張可能な設計とします。

---

## Open Questions

> [!NOTE]
> - 温度値にオフセット調整（例: ±1.5℃のズレ補正）やNVS保存の校正機能を追加する必要はありますか？（初期実装では B3950 標準計算式 $B=3950, R_0=10000$ で算出します）。

---

## Proposed Changes

### センサーモジュール (Sensor Module)

#### [NEW] [temp_sensor.h](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/temp_sensor.h)
#### [NEW] [temp_sensor.c](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/temp_sensor.c)
- `temp_sensor_data_t` 構造体の定義 (`raw_adc`, `voltage_mv`, `resistance_ohm`, `temp_celsius`)
- GPIO3 (`ADC1_CHANNEL_3`) の初期化処理 `temp_sensor_init(adc_oneshot_unit_handle_t adc_handle)`
- 10回平均読み取りと Bパラメータ式（Steinhart-Hart近似）による温度算出処理 `temp_sensor_read(...)`

#### [MODIFY] [CMakeLists.txt](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/CMakeLists.txt)
- ビルドソースに `temp_sensor.c` を追加。

---

### メインアプリケーション (Main Application)

#### [MODIFY] [cds_sensor.c](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/cds_sensor.c) & [cds_sensor.h](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/cds_sensor.h)
- `s_adc_handle` (ADC1ユニットハンドル) を外部参照可能にするか、Shared ADC1 管理関数を提供。

#### [MODIFY] [main.cpp](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/main.cpp)
- `temp_sensor_init()` を呼び出し。
- 周期計測タスクで `temp_sensor_read()` を呼び出し、最新の温度データを保持。

---

### Web サーバー & UI (Web Server & UI)

#### [MODIFY] [web_server.c](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/web_server.c)
- GET `/api/sensor` の JSON レスポンスに `temp_celsius`, `temp_raw_adc`, `temp_voltage_mv`, `temp_resistance_ohm` を追加。

#### [MODIFY] [web_page.h](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/main/web_page.h)
- ダッシュボードUIのセンサー表示エリアに **「🌡️ 温度」** カードを追加（デザインは既存の Glassmorphism UI と統一）。
- JavaScript の 1秒周期タイマーで `/api/sensor` から温度を取得し、リアルタイム更新。

---

### ドキュメント (Documentation)

#### [MODIFY] [README.md](file:///home/kusa/ドキュメント/eSp32/SwitchBot+sensor/README.md)
- ピン配置表に `GPIO3` (温度計測 B3950サーミスタ) を追記。
- REST API `/api/sensor` のレスポンス仕様更新。

---

## Verification Plan

### Automated Tests
- ESP-IDF ビルドの実行: `idf.py build` でコンパイルエラー・警告がないことを確認。

### Manual Verification
1. ESP32-C3 にファームウェアを書き込み (`idf.py -p /dev/ttyACM0 flash monitor`)。
2. シリアルログで GPIO3 からの ADC 読み取りと温度計算ログを確認。
3. Web UI (`http://esp32-switchbot.local/`) を開き、温度カードにリアルタイムの温度（例: `24.5 °C`）が表示されることを確認。
4. サーミスタを指で温めた際に温度表示が動的に上昇するか確認。
