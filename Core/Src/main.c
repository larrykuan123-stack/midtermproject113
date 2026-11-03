/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
// 定義有限狀態機 (FSM) 狀態
typedef enum {
    STATE_METRONOME_NORMAL = 0, // 模式 1: 一般節拍器模式
    STATE_METRONOME_TAP    = 1  // 模式 2: 節拍計算模式 (Tap Tempo)
} SystemState_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define DEBOUNCE_TIME_MS   50U    // 按鈕防彈跳時間 (50ms)
#define LED_FLASH_ON_MS    60U    // 每次節拍閃爍點亮時間 (60ms)
#define PWM_MAX_DUTY       800U   // PWM 閃爍亮度 (0 ~ 999)
#define TAP_SETTLE_MS      2000U  // 停手超過 2 秒未按按鈕，自動結算最終平均 BPM
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef hlpuart1;
TIM_HandleTypeDef htim3;

/* USER CODE BEGIN PV */
// --- FSM 狀態變數 ---
volatile SystemState_t current_state = STATE_METRONOME_NORMAL;

// --- 節拍器核心變數 (Non-blocking) ---
volatile uint32_t current_bpm = 120;         // 預設 120 BPM
volatile uint32_t target_period_ms = 500;    // 60000 / 120 = 500ms
uint32_t last_beat_tick = 0;                 // 上次節拍觸發時間點
uint8_t  beat_led_active = 0;                // 節拍 LED 是否處於點亮階段

// --- 誤差偵測變數 ---
volatile int32_t  system_period_error_ms = 0; // 系統排程週期誤差 (實測週期 - 理論週期)
volatile uint32_t actual_period_ms = 500;     // 實測閃爍週期
volatile int32_t  user_tap_error_ms = 0;      // 使用者在模式1跟拍的誤差

// --- EXTI 按鈕、防彈跳與連續打拍結算變數 ---
volatile uint8_t  btn_event_flag = 0;         // 中斷觸發旗標
volatile uint32_t btn_irq_tick = 0;           // 中斷當下時間戳記
uint32_t last_valid_btn_tick = 0;             // 上次有效按壓時間 (防彈跳紀錄)

uint32_t last_tap_calc_tick = 0;              // 模式2上次按按鈕的時間戳記
uint32_t tap_interval_sum = 0;                // 模式2累積的總間隔時間 (ms)
uint8_t  tap_count = 0;                       // 模式2目前連續按壓次數
uint8_t  tap_in_progress = 0;                 // 是否正在進行打拍輸入 (等待停手結算)

// --- LPUART1 非阻塞接收緩衝區 ---
uint8_t rx_byte;
char    rx_buffer[64];
uint8_t rx_index = 0;
volatile uint8_t cmd_ready_flag = 0;
char    tx_buffer[128];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_TIM3_Init(void);
/* USER CODE BEGIN PFP */
void Process_UART_Command(char *cmd);
void Update_FSM_Indicators(void);
void Send_UART_String(const char *str);
void Reset_Tap_Calculator(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// 發送字串輔助函式
void Send_UART_String(const char *str) {
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)str, strlen(str), 50);
}

// 重置模式 2 打拍累計器
void Reset_Tap_Calculator(void) {
    last_tap_calc_tick = 0;
    tap_interval_sum = 0;
    tap_count = 0;
    tap_in_progress = 0;
}

// 更新模式指示綠燈 (PB0): 模式 1 暗、模式 2 亮
void Update_FSM_Indicators(void) {
    if (current_state == STATE_METRONOME_TAP) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);   // 綠燈亮
    } else {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); // 綠燈暗
    }
}

// 解析來自 PySerial 的指令
void Process_UART_Command(char *cmd) {
    // 去除結尾的 \r 或 \n
    cmd[strcspn(cmd, "\r\n")] = 0;

    if (strcmp(cmd, "M1") == 0) {
        current_state = STATE_METRONOME_NORMAL;
        Reset_Tap_Calculator();
        Update_FSM_Indicators();
        snprintf(tx_buffer, sizeof(tx_buffer), "[FSM] Switched to Mode 1 (Normal Metronome) | BPM: %lu\r\n", current_bpm);
        Send_UART_String(tx_buffer);
    }
    else if (strcmp(cmd, "M2") == 0) {
        current_state = STATE_METRONOME_TAP;
        Reset_Tap_Calculator();
        Update_FSM_Indicators();
        snprintf(tx_buffer, sizeof(tx_buffer), "[FSM] Switched to Mode 2 (Tap Tempo) | Tap button multiple times, wait 2s to settle.\r\n");
        Send_UART_String(tx_buffer);
    }
    else if (strncmp(cmd, "BPM:", 4) == 0) {
        int new_bpm = atoi(cmd + 4);
        if (new_bpm >= 30 && new_bpm <= 300) {
            current_bpm = (uint32_t)new_bpm;
            target_period_ms = 60000UL / current_bpm;
            snprintf(tx_buffer, sizeof(tx_buffer), "[SET] Target BPM: %lu (Period: %lu ms)\r\n", current_bpm, target_period_ms);
            Send_UART_String(tx_buffer);
        } else {
            Send_UART_String("[WARN] BPM out of range (Valid: 30~300)\r\n");
        }
    }
    else if (strcmp(cmd, "ERR") == 0) {
        uint32_t actual_bpm_int = (actual_period_ms > 0) ? (60000UL / actual_period_ms) : 0;
        int32_t bpm_err_int = (int32_t)actual_bpm_int - (int32_t)current_bpm;
        snprintf(tx_buffer, sizeof(tx_buffer),
                 "[ERROR REPORT] Target: %lu BPM (%lu ms) | Actual: %lu BPM (%lu ms) | SysErr: %ld ms (%ld BPM) | TapErr: %ld ms\r\n",
                 current_bpm, target_period_ms, actual_bpm_int, actual_period_ms,
                 system_period_error_ms, bpm_err_int, user_tap_error_ms);
        Send_UART_String(tx_buffer);
    }
    else {
        snprintf(tx_buffer, sizeof(tx_buffer), "[UNKNOWN CMD] %s (Use M1, M2, BPM:xxx, ERR)\r\n", cmd);
        Send_UART_String(tx_buffer);
    }
}

// EXTI 外部中斷 Callback (處理 PC13 按鈕觸發)
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == GPIO_PIN_13) {
        btn_irq_tick = HAL_GetTick();
        btn_event_flag = 1; // 交由主迴圈做非阻塞防彈跳與邏輯處理
    }
}

// UART 非阻塞接收 Callback
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == LPUART1) {
        if (rx_byte == '\n') {
            rx_buffer[rx_index] = '\0';
            rx_index = 0;
            cmd_ready_flag = 1; // 通知主迴圈解析指令
        } else {
            if (rx_index < sizeof(rx_buffer) - 1) {
                rx_buffer[rx_index++] = (char)rx_byte;
            }
        }
        HAL_UART_Receive_IT(&hlpuart1, &rx_byte, 1);
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LPUART1_UART_Init();
  MX_TIM3_Init();

  /* USER CODE BEGIN 2 */
  // 強制將 LPUART1 鮑率設為 115200
  hlpuart1.Init.BaudRate = 115200;
  HAL_UART_Init(&hlpuart1);

  // 強制將 TIM3 週期設為 999 (1000-1)
  __HAL_TIM_SET_AUTORELOAD(&htim3, 999);

  // 1. 啟動 TIM3_CH1 (PA6) 的 PWM 輸出
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, PWM_MAX_DUTY);

  // 2. 初始化模式指示綠燈 (預設 Mode 1 熄滅)
  Update_FSM_Indicators();

  // 3. 啟動 LPUART1 非阻塞中斷接收
  HAL_UART_Receive_IT(&hlpuart1, &rx_byte, 1);

  last_beat_tick = HAL_GetTick();
  beat_led_active = 1;
  Send_UART_String("=== STM32G474 Smart Metronome Ready ===\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      uint32_t now = HAL_GetTick();

      // =========================================================
      // 1. 非阻塞處理 LPUART1 指令
      // =========================================================
      if (cmd_ready_flag) {
          cmd_ready_flag = 0;
          Process_UART_Command(rx_buffer);
      }

      // =========================================================
      // 2. 非阻塞按鈕防彈跳 (Debounce) 與打拍收集
      // =========================================================
      if (btn_event_flag) {
          btn_event_flag = 0;
          // 軟體防彈跳：距離上次有效按壓必須大於 DEBOUNCE_TIME_MS (50ms)
          if ((btn_irq_tick - last_valid_btn_tick) >= DEBOUNCE_TIME_MS) {
              last_valid_btn_tick = btn_irq_tick;

              switch (current_state) {
                  case STATE_METRONOME_NORMAL: {
                      // 模式 1：計算使用者跟拍時間與最近一次節拍點的誤差
                      uint32_t elapsed_since_beat = btn_irq_tick - last_beat_tick;
                      if (elapsed_since_beat <= (target_period_ms / 2)) {
                          user_tap_error_ms = (int32_t)elapsed_since_beat; // 偏慢 (+)
                      } else {
                          user_tap_error_ms = (int32_t)elapsed_since_beat - (int32_t)target_period_ms; // 偏快 (-)
                      }
                      break;
                  }

                  case STATE_METRONOME_TAP: {
                      // 模式 2：只收集每次按壓間隔，不立即回傳洗版
                      if (tap_count == 0) {
                          // 第一下按壓：啟動計時
                          tap_count = 1;
                          tap_interval_sum = 0;
                          tap_in_progress = 1;
                      } else {
                          uint32_t interval_ms = btn_irq_tick - last_tap_calc_tick;
                          // 檢查單次間隔是否在合理範圍 (30 ~ 300 BPM 對應 200ms ~ 2000ms)
                          if (interval_ms >= 200 && interval_ms <= 2000) {
                              tap_interval_sum += interval_ms;
                              tap_count++;
                          } else {
                              // 若間隔異常，將這一下視為新的第一下重新計算
                              tap_count = 1;
                              tap_interval_sum = 0;
                          }
                      }
                      last_tap_calc_tick = btn_irq_tick;

                      // 打拍當下讓節拍燈跟著閃一下作為視覺回饋
                      last_beat_tick = now;
                      beat_led_active = 1;
                      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, PWM_MAX_DUTY);
                      break;
                  }
              }
          }
      }

      // =========================================================
      // 3. 模式 2 停手自動結算機制 (超過 2 秒未繼續按壓才結算並回報)
      // =========================================================
      if (current_state == STATE_METRONOME_TAP && tap_in_progress) {
          if ((now - last_tap_calc_tick) >= TAP_SETTLE_MS) {
              if (tap_count >= 2) {
                  // 計算平均間隔 = 總間隔時間 / (按壓次數 - 1)
                  uint32_t avg_interval_ms = tap_interval_sum / (tap_count - 1);
                  current_bpm = 60000UL / avg_interval_ms;
                  target_period_ms = avg_interval_ms;

                  // 停手後只回報這一次最終結算結果！
                  snprintf(tx_buffer, sizeof(tx_buffer),
                           "[TAP RESULT] Taps: %u | Avg Interval: %lu ms -> Final BPM: %lu\r\n",
                           tap_count, avg_interval_ms, current_bpm);
                  Send_UART_String(tx_buffer);
              } else {
                  // 若只按了 1 下就停手，提示次數不足並放棄本次輸入
                  Send_UART_String("[TAP CANCEL] Only 1 tap detected, need at least 2 taps.\r\n");
              }
              // 結算完畢，重置打拍累計器，等待下一輪打拍
              Reset_Tap_Calculator();
          }
      }

      // =========================================================
      // 4. 非阻塞 PWM 節拍器閃爍與誤差偵測引擎
      // =========================================================
      uint32_t elapsed = now - last_beat_tick;

      // 在正在打拍輸入的過程中，暫停原本自動固定週期的閃爍，改由按鈕同步點亮；停手結算後立即恢復新 BPM 閃爍
      if (!tap_in_progress && (elapsed >= target_period_ms)) {
          actual_period_ms = elapsed;
          system_period_error_ms = (int32_t)actual_period_ms - (int32_t)target_period_ms;
          last_beat_tick = now;

          beat_led_active = 1;
          __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, PWM_MAX_DUTY); // PWM 點亮節拍燈
      }
      else if (beat_led_active && (elapsed >= LED_FLASH_ON_MS)) {
          beat_led_active = 0;
          __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0);            // PWM 熄滅節拍燈
      }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 169;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_TIM_MspPostInit(&htim3);
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
