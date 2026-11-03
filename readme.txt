# 🎵 STM32 智慧節拍器 (Smart Metronome)

基於 **NUCLEO-G474RE (STM32G474RE)** 開發板設計的嵌入式智慧節拍器專題，結合硬體外設控制、有限狀態機 (FSM)、Non-blocking 非阻塞定時與 Python (`pyserial`) 雙向序列埠互動。

---

## 🚀 系統功能特色

1. **一般節拍器模式 (Mode 1)**：
   * 透過 Python 介面輸入指定 BPM（支援 30 ~ 300 BPM）。
   * 利用硬體定時器 PWM 驅動外接 LED 進行精準節拍閃爍。
   * 支援即時跟拍誤差偵測，並在收到 `ERR` 指令時回傳系統週期誤差與使用者跟拍誤差。
2. **節拍計算模式 (Tap Tempo, Mode 2)**：
   * 切換至此模式時，外接綠色指示燈亮起。
   * 透過連續按下板載按鈕（PC13）輸入節拍，系統自動累加計算間隔，並在停手滿 2 秒後自動平均結算最終 BPM，透過 UART 回報並同步更新 LED 閃爍速度。
3. **UART 互動控制**：
   * 透過 ST-LINK VCP（`LPUART1` / `115200` bps）與電腦端 Python 程式進行非阻塞雙向通訊。

---

## 🛠️ 硬體架構與接腳規劃 (Pinout)

| 功能模組 | MCU 腳位 | 硬體位置 / 排針 | CubeMX 設定 | 說明 |
| :--- | :--- | :--- | :--- | :--- |
| **節拍閃爍 LED** | **PA6** | D12 (CN10-13) | `TIM3_CH1` (PWM) | 串聯 $220\Omega$ 電阻接 LED 正極，負極接 GND |
| **模式指示綠燈** | **PB0** | A3 (CN8-7) | `GPIO_Output` | 模式 2 亮、模式 1 暗 |
| **打拍/跟拍按鈕** | **PC13** | 板載按鈕 B1 | `GPIO_EXTI13` (中斷) | 內建上拉，具備 50ms 軟體防彈跳 |
| **UART 通訊** | **PA2 / PA3** | ST-LINK VCP | `LPUART1` (非阻塞中斷) | 透過 USB 線直連電腦 |

---

## 📂 專案檔案結構

* `Core/Src/main.c`：STM32 韌體核心程式（含 FSM、PWM、EXTI 斷線與 UART 解析）。
* `metronome_host.py`：Python 端控制介面（含背景執行緒監聽與自動 COM Port 搜尋）。

---

## 💻 軟體需求與環境設定

* **韌體開發**：STM32CubeMX、STM32CubeIDE
* **電腦端控制**：Python 3.x 與 `pyserial` 套件
  ```bash
  pip install pyserial