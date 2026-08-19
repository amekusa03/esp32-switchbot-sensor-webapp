# ESP32-C3 SwitchBot Smart Home & smartOptSensor Hub

ESP32-C3 上で動作する高機能 Web サーバー経由で SwitchBot Open API v1.1 を制御し、同時に CdS 照度センサーによる照度計測および Matter Over Wi-Fi Light Sensor サービスを並行稼働させる統合システムです。

---

## 🌟 特長 (Features)

- **ESP32-C3 マイコン対応**: ESP32-C3 ターゲットに最適化されたファームウェア構成。
- **smartOptSensor 照度計測機能**:
  - **ピン配置**: `GPIO1` (ADC1_CH1) にて CdS セルと 約9.1kΩ プルダウン抵抗(100kΩ + 10kΩ並列)による分圧回路を 1,000ms 周期で高精度サンプリング（10回分散平均）。
  - **Lux & Matter 換算**: ADC生値・キャリブレーション電圧から CdS 抵抗値、Lux（照度）、および Matter 規格の `MeasuredValue`（`10000 * log10(lux) + 1`）を自動算出してシリアルログおよび Web UI にリアルタイム表示。
  - **Web UI 校正機能**: Web ダッシュボード上の「🔧 センサー校正」パネルから 2 点実測校正を実施。校正値（Point 1 / Point 2）は NVS に永続保存され、再起動後も自動で適用される。
- **Matter Over Wi-Fi 対応**: Matter Light Sensor Endpoint (`0x0106`, Cluster `0x0400`) を提供し、Google Home などのスマートホームエコシステムから照度センサーとして認識・利用可能。
- **ファクトリーリセット**: `GPIO9` ボタンを 3 秒以上長押しすることで Matter および NVS 設定のリセットを実行可能。
- **SwitchBot Open API v1.1 HMAC-SHA256 認証実装**: `mbedtls` を使用して `token`, `secret`, ミリ秒精度 `t`, ランダム `nonce` による暗号署名を生成。
- **NTP 時刻同期**: `esp_netif_sntp` で世界標準時を取得し、正確なタイムスタンプで API リクエストを送信。
- **レスポンシブ Glassmorphism UI**: 照度センサーのリアルタイム表示バナーと SwitchBot 各種家電操作カードを一体化したデザイン。
- **mDNS (Bonjour) 対応**: `http://esp32-switchbot.local/` でアクセス可能。IPアドレスを覚える必要なし。Matter の mDNS と共存する delegate hostname 方式を採用。
- **SoftAP フォールバック & NVS 永続化**: Wi-Fi未設定時または接続失敗時は自動的に AP モード (`ESP32-SwitchBot-Setup`, `192.168.4.1`) で起動。

---

## 🔌 ピン配置 (Pin Mapping)

| ESP32-C3 ピン | 接続先 | 役割 |
| :--- | :--- | :--- |
| **3.3V** | CdS セルの片足 | 電源供給 |
| **GPIO1 (ADC1_CH1)** | CdSセルとプルダウン抵抗(約9.1kΩ)の接続点 | ADC照度計測入力 |
| **GND** | プルダウン抵抗(約9.1kΩ)の片足 | グランド |
| **GPIO9** | タクトスイッチ（GNDへ短絡） | 3秒長押しでファクトリーリセット |

---

## 🛠 ビルドおよび書き込み方法 (Build & Flash)

### 1. 環境変数のロード

```bash
source /home/kusa/esp/esp-idf/export.sh
source /home/kusa/esp/esp-matter/export.sh
```

### 2. ターゲットを esp32c3 に設定

```bash
cd /home/kusa/ドキュメント/eSp32/SwitchBot+sensor
idf.py set-target esp32c3
```

### 3. プロジェクトのビルド

```bash
idf.py build
```

### 4. ESP32-C3 への書き込みとシリアルモニタ

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

---

## 🌐 ネットワークアクセス (Network Access)

Wi-Fi 接続後、以下のいずれかの URL でダッシュボードにアクセスできます。

| 方法 | URL | 備考 |
| :--- | :--- | :--- |
| **mDNS (推奨)** | `http://esp32-switchbot.local/` | iOS / macOS / Windows 10以降 / Linux (Avahi) で対応 |
| **IPアドレス直接** | `http://<IPアドレス>/` | シリアルモニタで確認可能 |
| **SoftAP モード** | `http://192.168.4.1/` | 初期設定時・Wi-Fi未接続時 |

> **💡 Linux で mDNS を使う場合**: `avahi-daemon` と `libnss-mdns` のインストールが必要です。
> ```bash
> sudo apt install avahi-daemon libnss-mdns
> ```

---

## 📡 REST API 仕様 (Local Endpoints)

| エンドポイント | メソッド | 説明 |
| --- | --- | --- |
| `/` | `GET` | WebダッシュボードUIの配信 |
| `/api/status` | `GET` | IPアドレス、WiFi電波強度(RSSI)、メモリ、NTP同期状態 |
| `/api/sensor` | `GET` | CdS照度センサー情報取得 (`raw_adc`, `voltage_mv`, `resistance_ohm`, `lux`, `matter_value`) |
| `/api/config` | `POST` | Wi-FiおよびSwitchBot認証情報のNVS保存 |
| `/api/devices` | `GET` | SwitchBot Cloudから全デバイス・IRリモコン一覧を取得 |
| `/api/device/command` | `POST` | デバイスへの操作コマンド送信 (`{"deviceId":"...","command":"turnOn"}`) |
| `/api/scenes` | `GET` | 登録済みシーン一覧の取得 |
| `/api/scene/execute` | `POST` | シーンの実行 (`{"sceneId":"..."}`) |
| `/api/sensor/calibrate` | `GET` | 現在の CdS 校正パラメータ取得 (`raw1`, `lux1`, `raw2`, `lux2`, `A`, `R_ref`, `n`) |
| `/api/sensor/calibrate` | `POST` | 校正点の記録 (`{"action":"set_point","point":1,"lux":13.0}`) または NVS保存 (`{"action":"save"}`) |

---

## 🔧 CdS センサー校正 (Sensor Calibration)

照度計測の精度を高めるため、Web UI から 2 点実測校正を行えます。校正値は **NVS（不揮発性ストレージ）に保存** され、電源オフ・再起動後も自動で引き継がれます。

### 校正手順

1. ブラウザで `http://esp32-switchbot.local/` を開く
2. センサーカード右下の **「校正 ▼」** ボタンをクリック
3. **Point 1（明るい側）**: 照度計で実測した Lux 値を入力し「現在値で記録」を押す
4. **Point 2（暗い側）**: 暗い環境に変えて同様に「現在値で記録」を押す
5. **「💾 NVSに保存」** を押して確定する

### 校正モデル

```
lux = A × (R_ref / R_cds)^n
```

| パラメータ | 説明 |
| :--- | :--- |
| `A` | 基準照度（Point 1 の Lux 値） |
| `R_ref` | Point 1 の CdS 抵抗値 (Ω) |
| `n` | べき乗指数（2 点の比から自動算出） |

### NVS キー (namespace: `sb_storage`)

| キー | 型 | 説明 |
| :--- | :--- | :--- |
| `cds_raw1` | i32 | Point 1 の ADC 生値 |
| `cds_lux1` | blob (float) | Point 1 の実測照度 (Lux) |
| `cds_raw2` | i32 | Point 2 の ADC 生値 |
| `cds_lux2` | blob (float) | Point 2 の実測照度 (Lux) |

> **フォールバック**: NVS 未書き込みの場合はデフォルト値 (RAW 1300 = 13 Lux / RAW 70 = 0.1 Lux) で動作します。
