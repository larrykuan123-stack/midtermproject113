import serial
import serial.tools.list_ports
import threading
import time

BAUD_RATE = 115200

def find_serial_port():
    """自動列出電腦上所有 COM Port，並優先尋找 STM32 ST-Link"""
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("❌ 電腦目前沒有偵測到任何 COM Port！請檢查 STM32 的 USB 線是否有插緊。")
        return None

    print("\n--- 偵測到以下連接埠 ---")
    stm32_port = None
    for p in ports:
        print(f"  * {p.device} : {p.description}")
        if "STLink" in p.description or "STMicroelectronics" in p.description:
            stm32_port = p.device
    print("------------------------")

    if stm32_port:
        print(f"✅ 自動找到 STM32 開發板：{stm32_port}")
        return stm32_port
    else:
        user_port = input("請手動輸入上方顯示的 COM 編號 (例如 COM3): ").strip().upper()
        return user_port

def rx_listener(ser):
    """背景執行緒：持續接收 STM32 傳來的訊息"""
    while ser.is_open:
        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(f"\n<<< [STM32] {line}")
                    print("輸入指令 (M1 / M2 / BPM:數值 / ERR / q離開) > ", end="", flush=True)
        except Exception:
            break
        time.sleep(0.01)

def main():
    port_name = find_serial_port()
    if not port_name:
        input("\n按 Enter 鍵關閉視窗...")
        return

    try:
        ser = serial.Serial(port_name, BAUD_RATE, timeout=0.1)
        print(f"🎉 成功連線至 {port_name} ({BAUD_RATE} bps)！")
    except Exception as e:
        print(f"\n❌ 無法開啟序列埠 {port_name}，錯誤原因：{e}")
        print("💡 提示：請確認沒有其他程式占用該 COM Port，且名稱輸入正確。")
        input("\n按 Enter 鍵關閉視窗...")
        return

    # 啟動背景接收執行緒
    thread = threading.Thread(target=rx_listener, args=(ser,), daemon=True)
    thread.start()

    print("\n================ 智慧節拍器操作說明 ================")
    print("  M1       : 切換至【模式 1：一般節拍器模式】(綠燈滅)")
    print("  M2       : 切換至【模式 2：節拍計算模式 Tap Tempo】(綠燈亮)")
    print("  BPM:120  : 指定節拍速度 (範圍 30 ~ 300)")
    print("  ERR      : 索取目前節拍週期誤差與跟拍誤差報告")
    print("  q        : 結束程式")
    print("====================================================\n")

    try:
        while True:
            cmd = input("輸入指令 (M1 / M2 / BPM:數值 / ERR / q離開) > ").strip()
            if not cmd:
                continue
            if cmd.lower() == 'q':
                break
            ser.write((cmd + '\n').encode('utf-8'))
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        input("\n已關閉序列埠連線。按 Enter 鍵關閉視窗...")

if __name__ == '__main__':
    main()