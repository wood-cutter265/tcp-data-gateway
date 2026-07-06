/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

osThreadId defaultTaskHandle;
osThreadId wifiTaskHandle;
osThreadId sensorUploadTasHandle;
/* USER CODE BEGIN PV */

// ========== WiFi & 缃戝叧閰嶇疆锛堟敼鎴愪綘鑷繁鐨勶級==========
#define WIFI_SSID     "TP-LINK_5G"
#define WIFI_PASS     "12345678"
#define GW_IP         "10.45.14.150"
#define GW_PORT       8888

// ========== 鍏ㄥ眬鍏变韩鍙橀噺 ==========
volatile int g_wifi_connected = 0;      // WiFi+TCP 杩炰笂鍚庣疆1
static uint32_t g_data_id = 0;          // 鏁版嵁娴佹按鍙?
static uint8_t  g_esp_rx_buf[256];      // ESP8266 搴旂瓟鎺ユ敹缂撳啿

// ========== 8瀛楄妭鏁版嵁鍖咃紙涓庝笂浣嶆満 NetPacket 涓ユ牸瀵归綈锛?==========
typedef struct {
    uint32_t data_id;
    uint32_t payload;
} __attribute__((packed)) SensorPacket;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void StartDefaultTask(void const * argument);
void wifi_task(void const * argument);
void sensor_upload_task(void const * argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// ====================================================================
// 璇讳竴涓? UART 瀛楄妭锛堝瘎瀛樺櫒鏂瑰紡锛屾瘮 HAL_UART_Receive 蹇緢澶氾級
// 杩斿洖 1 琛ㄧず璇诲埌锛?0 琛ㄧず娌℃暟鎹?
// ====================================================================
static int uart_read_byte(uint8_t *ch) {
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE)) {
        *ch = (uint8_t)(huart1.Instance->DR & 0xFF);
        return 1;
    }
    return 0;
}

// ====================================================================
// 娓呯┖ UART1 鎺ユ敹缂撳啿锛堜涪寮冨瀮鍦炬暟鎹級
// ====================================================================
static void esp_flush_rx(void) {
    uint8_t dummy;
    while (uart_read_byte(&dummy));
}

// ====================================================================
// 浠? UART1 璇诲彇 ESP8266 鍝嶅簲锛屾鏌ユ槸鍚﹀寘鍚湡鏈涘瓧绗︿覆
// 杩斿洖:1=鎵惧埌  0=瓒呮椂/鍑洪敊
//
// 鍏抽敭璁捐锛氬厛璇诲畬鍏ㄩ儴鍙敤瀛楄妭锛屽啀鏌? strstr锛岄伩鍏? strstr 杩囨參涓㈠瓧绗?
// ====================================================================
static int esp_wait_resp(const char *expect, uint32_t timeout_ms) {
    memset(g_esp_rx_buf, 0, sizeof(g_esp_rx_buf));
    uint32_t start = HAL_GetTick();
    uint16_t idx = 0;
    int expect_len = strlen(expect);

    while (HAL_GetTick() - start < timeout_ms) {
        // 鍐呭眰寰幆锛氬厛璇诲厜鎵?鏈夊瓧鑺傦紝涓嶅仛 strstr
        uint8_t ch;
        while (uart_read_byte(&ch)) {
            if (idx < sizeof(g_esp_rx_buf) - 1) {
                g_esp_rx_buf[idx++] = ch;
            }
        }
        g_esp_rx_buf[idx] = '\0';

        // 澶栧眰鍐嶆鏌ワ細缂撳啿澶熼暱浜嗘墠鏌? strstr
        if (idx >= expect_len) {
            if (strstr((char*)g_esp_rx_buf, expect))
                return 1;
            if (strstr((char*)g_esp_rx_buf, "ERROR") ||
                strstr((char*)g_esp_rx_buf, "FAIL"))
                return 0;
        }
    }
    return 0;   // 瓒呮椂
}

// ====================================================================
// 鍙戦?? AT 鍛戒护锛岀瓑寰呮湡鏈涘洖搴?
// ====================================================================
static int esp_cmd(const char *cmd, const char *expect, uint32_t timeout_ms) {
    esp_flush_rx();
    HAL_UART_Transmit(&huart1, (uint8_t*)cmd, strlen(cmd), 100);
    return esp_wait_resp(expect, timeout_ms);
}

// ====================================================================
// 杩炴帴 WiFi
// ====================================================================
static int wifi_connect(void) {
    char buf[128];
    uint8_t msg[] = "[WIFI] connecting...\r\n";
    HAL_UART_Transmit(&huart2, msg, sizeof(msg)-1, 100);

    // 鍏冲洖鏄? + 璁? Station 妯″紡
    esp_cmd("ATE0\r\n", "OK", 500);
    esp_cmd("AT+CWMODE=1\r\n", "OK", 500);
    // 杩? WiFi锛堟渶闀跨瓑 25 绉掞紝NodeMCU 鎴愬姛鍚庡洖 OK锛?
    snprintf(buf, sizeof(buf), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    esp_flush_rx();
    HAL_UART_Transmit(&huart1, (uint8_t*)buf, strlen(buf), 100);
    int r = esp_wait_resp("OK", 25000);
    if (r) {
        uint8_t ok[] = "[WIFI] OK\r\n";
        HAL_UART_Transmit(&huart2, ok, sizeof(ok)-1, 100);
    } else {
        uint8_t dbg[] = "[WIFI] FAIL, raw: ";
        HAL_UART_Transmit(&huart2, dbg, sizeof(dbg)-1, 100);
        HAL_UART_Transmit(&huart2, g_esp_rx_buf, strlen((char*)g_esp_rx_buf), 100);
        HAL_UART_Transmit(&huart2, (uint8_t*)"\r\n", 2, 100);
    }
    return r;
}

// ====================================================================
// TCP 杩炴帴鍒扮綉鍏?
// ====================================================================
static int tcp_connect(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", GW_IP, GW_PORT);
    esp_flush_rx();
    HAL_UART_Transmit(&huart1, (uint8_t*)buf, strlen(buf), 100);
    int r = esp_wait_resp("CONNECT", 8000);
    if (r) {
        uint8_t ok[] = "[TCP] connected\r\n";
        HAL_UART_Transmit(&huart2, ok, sizeof(ok)-1, 100);
    } else {
        uint8_t dbg[] = "[TCP] FAIL, raw: ";
        HAL_UART_Transmit(&huart2, dbg, sizeof(dbg)-1, 100);
        HAL_UART_Transmit(&huart2, g_esp_rx_buf, strlen((char*)g_esp_rx_buf), 100);
        HAL_UART_Transmit(&huart2, (uint8_t*)"\r\n", 2, 100);
    }
    return r;
}

// ====================================================================
// 鍙戦?? 8 瀛楄妭鏁版嵁鍖?
// ====================================================================
static int send_packet(const SensorPacket *pkt) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "AT+CIPSEND=%d\r\n", (int)sizeof(SensorPacket));
    if (!esp_cmd(buf, ">", 2000)) return 0;         // 绛? '>' 鎻愮ず绗?

    HAL_UART_Transmit(&huart1, (uint8_t*)pkt, sizeof(SensorPacket), 100);
    return esp_wait_resp("SEND OK", 3000);           // 纭鍙戦?佹垚鍔?
}

// ====================================================================
// 璇诲彇鍐呴儴娓╁害浼犳劅鍣紙鏀惧ぇ100鍊?: 2550 = 25.50掳C锛?
// ====================================================================
static int32_t read_temp_c100(void) {
    // 鐢ㄩ暱閲囨牱鏃堕棿鎻愰珮娓╁害璇绘暟绮惧害
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel = ADC_CHANNEL_TEMPSENSOR;
    ch.Rank = 1;
    ch.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    HAL_ADC_ConfigChannel(&hadc1, &ch);

    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 100);
    uint32_t adc_val = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    // F103 鍐呴儴娓╁害鍏紡(杩戜技): T = 1430 - 3300*adc/4096   (mV)
    //                          鐒跺悗 (V25 - Vsense) / 4.3 + 25  (掳C)
    // 鍏ㄩ儴鏁存暟杩愮畻閬垮厤娴偣锛岀粨鏋滄斁澶т簡100鍊?
    int32_t vsense_mv = (int32_t)adc_val * 3300 / 4096;
    int32_t t100 = ((1430 - vsense_mv) * 100 / 43) + 2500;
    return t100;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* definition and creation of wifiTask */
  osThreadDef(wifiTask, wifi_task, osPriorityNormal, 0, 256);
  wifiTaskHandle = osThreadCreate(osThread(wifiTask), NULL);

  /* definition and creation of sensorUploadTas */
  osThreadDef(sensorUploadTas, sensor_upload_task, osPriorityNormal, 0, 256);
  sensorUploadTasHandle = osThreadCreate(osThread(sensorUploadTas), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */
  // 寮?鍚唴閮ㄦ俯搴︿紶鎰熷櫒(TSVREFE浣?)
  hadc1.Instance->CR2 |= ADC_CR2_TSVREFE;
  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_wifi_task */
/**
* @brief Function implementing the wifiTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_wifi_task */
void wifi_task(void const * argument)
{
  /* USER CODE BEGIN wifi_task */
  for(;;)
  {
      // ========== 闃舵1: 杩? WiFi ==========
      HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);  // LED浜?
      while (!wifi_connect()) {
          HAL_UART_Transmit(&huart2, (uint8_t*)"[WIFI] retry...\r\n", 17, 100);
          osDelay(3000);
      }

      // ========== 闃舵2: 杩? TCP ==========
      while (!tcp_connect()) {
          HAL_UART_Transmit(&huart2, (uint8_t*)"[TCP] retry...\r\n", 16, 100);
          osDelay(3000);
      }

      g_wifi_connected = 1;      // 閫氱煡 sensor 浠诲姟鍙互鍙戜簡
      HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);    // LED鐏?

      // ========== 闃舵3: 淇濇寔杩炴帴 ==========
      while (g_wifi_connected) {
          // 姣? 5 绉掓鏌ヤ竴娆¤繛鎺ョ姸鎬?
          if (!esp_cmd("AT+CIPSTATUS\r\n", "STATUS:3", 1000)) {
              g_wifi_connected = 0;   // 涓簡锛屽洖鍒板紑澶撮噸杩?
              HAL_UART_Transmit(&huart2, (uint8_t*)"[TCP] lost\r\n", 12, 100);
          }
          osDelay(5000);
      }
  }
  /* USER CODE END wifi_task */
}

/* USER CODE BEGIN Header_sensor_upload_task */
/**
* @brief Function implementing the sensorUploadTas thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_sensor_upload_task */
void sensor_upload_task(void const * argument)
{
  /* USER CODE BEGIN sensor_upload_task */
  for(;;)
  {
      // 等 WiFi 连好
      if (!g_wifi_connected) {
          osDelay(500);
          continue;
      }

      // 1. 读内部温度（×100：2634 = 26.34°C）
      int32_t t100 = read_temp_c100();
      if (t100 < 0)    t100 = 0;
      if (t100 > 9999) t100 = 9999;

      // 2. 发送温度值
      SensorPacket pkt;
      pkt.data_id = ++g_data_id;
      pkt.payload = (uint32_t)t100;
      if (send_packet(&pkt)) {
          char dbg[64];
          int len = snprintf(dbg, sizeof(dbg),
              "[UP] ID=%lu Temp=%ld.%02ldC\r\n",
              (unsigned long)pkt.data_id,
              (long)(t100 / 100), (long)(t100 % 100));
          HAL_UART_Transmit(&huart2, (uint8_t*)dbg, len, 100);
      }

      // LED 闪一下表示发送成功
      HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

      // 3. 每隔 5 秒采集一次
      osDelay(5000);
  }
  /* USER CODE END sensor_upload_task */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
