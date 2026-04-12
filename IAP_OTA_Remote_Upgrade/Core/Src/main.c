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
#include "crc.h"
#include "dma.h"
#include "lwip.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "protocol.h"
#include "flash_manage.h"
#include "crc16.h"
#include "protocol_handler.h"
#include "usart.h"
#include "eth_tcp_server.h"
#include "lwip/netif.h"
#include "wifi_at.h"
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

/* USER CODE BEGIN PV */

extern uint8_t g_uart1_rx_byte;
extern uint8_t g_uart3_rx_byte;
volatile uint8_t g_stay_in_bootloader = 0U;
volatile uint8_t g_update_finish = 0U;
volatile UpdateMethod_t g_update_method = no_update; 
static ProtocolRxBuffer_t g_eth_protocol_rx_buf;
static uint8_t s_eth_prev_connected = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint8_t Boot_CopySelectedAppToRun(void);
static uint8_t Boot_IsRunAppValid(void);
static void Boot_JumpToRunApp(void);
static uint8_t Boot_CopyRunAndJump(FlashParam_t *param, const char *copy_fail_log);
static void Eth_OnBytes(const uint8_t *data, uint16_t len);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint8_t Boot_CopySelectedAppToRun(void)
{
    uint8_t ret;
    FlashParam_t param;
    AppArea_t target = APP_AREA_NONE;
    uint32_t src_addr = 0U;
    uint32_t copy_len = 0U;

    if (Param_Load(&param) != 0U) {
        return 1U;
    }

    if (param.boot_select == APP_AREA_A && param.app_a_status == APP_STATUS_VALID) {
        target = APP_AREA_A;
    } else if (param.boot_select == APP_AREA_B && param.app_b_status == APP_STATUS_VALID) {
        target = APP_AREA_B;
    } else if (param.app_a_status == APP_STATUS_VALID) {
        target = APP_AREA_A;
    } else if (param.app_b_status == APP_STATUS_VALID) {
        target = APP_AREA_B;
    } else {
        printf("Error: No valid application found.\r\n");
        return 1U;
    }

    if (target == APP_AREA_A) {
        src_addr = APP_A_SECTOR_START_ADDR;
        copy_len = param.app_a_size;
        param.run_app_version = param.app_a_version;
        param.run_app_size = param.app_a_size;
        param.run_app_crc32 = param.app_a_crc32;
    } else {
        src_addr = APP_B_SECTOR_START_ADDR;
        copy_len = param.app_b_size;
        param.run_app_version = param.app_b_version;
        param.run_app_size = param.app_b_size;
        param.run_app_crc32 = param.app_b_crc32;
    }

    if (copy_len == 0U || (copy_len % 4U) != 0U) {
        return 1U;
    }

    ret = Flash_Copy(RUN_SECTOR_START_ADDR, src_addr, copy_len);
    if (ret != 0U) {
        return ret;
    }

    param.run_app_status = APP_STATUS_VALID;
    param.boot_select = target;
    if (Param_Save(&param) != 0U) {
        return 1U;
    }

    return 0U;
}

static uint8_t Boot_IsRunAppValid(void)
{
    uint32_t msp = *(volatile uint32_t *)RUN_SECTOR_START_ADDR;
    uint32_t reset = *(volatile uint32_t *)(RUN_SECTOR_START_ADDR + 4U);

    if ((msp & 0x2FFE0000U) != 0x20000000U) {
        return 1U;
    }

    if (reset < RUN_SECTOR_START_ADDR || reset > RUN_SECTOR_END_ADDR) {
        return 1U;
    }

    FlashParam_t param;
    if (Param_Load(&param) == 0U) {
        if (param.run_app_size > 0U && (param.run_app_size % 4U) == 0U &&
            (RUN_SECTOR_START_ADDR + param.run_app_size - 1U) <= RUN_SECTOR_END_ADDR) {
            extern CRC_HandleTypeDef hcrc;
            __HAL_CRC_DR_RESET(&hcrc);
            uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)RUN_SECTOR_START_ADDR, param.run_app_size / 4U);
            if (crc != param.run_app_crc32) {
                return 1U;
            }
        }
    }

    return 0U;
}

static void Boot_JumpToRunApp(void)
{
    uint32_t app_stack = *(volatile uint32_t *)RUN_SECTOR_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(RUN_SECTOR_START_ADDR + 4U);

    typedef void (*pFunc)(void);
    pFunc Jump = (pFunc)app_reset;

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    // 可选：清NVIC使能/挂起，避免遗留中断影响App
    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    // 关键：把向量表切到RunApp
    SCB->VTOR = RUN_SECTOR_START_ADDR;

    __set_MSP(app_stack);
    Jump();

    while (1) { }
}

  static uint8_t Boot_CopyRunAndJump(FlashParam_t *param, const char *copy_fail_log)
  {
    if (param == NULL) {
      return 1U;
    }

    if (Boot_CopySelectedAppToRun() == 0U) {
      printf("    Copy to RunApp success...\r\n");
      if (Boot_IsRunAppValid() == 0U) {
        Boot_JumpToRunApp();
        return 0U;
      }

      param->run_app_status = APP_STATUS_INVALID;
      if (Param_Save(param) != 0U) {
        printf("    Save param failed...\r\n");
        return 1U;
      }
      printf("    RunApp invalid/CRC failed/Vector invalid...\r\n");
    } else {
      printf("%s", copy_fail_log);
    }

    return 1U;
  }
/* 以太网接收回调 
extern struct netif gnetif;
extern uint8_t IP_ADDRESS[4];
extern uint8_t NETMASK_ADDRESS[4];
extern uint8_t GATEWAY_ADDRESS[4];

static void Print_NetInfo(void)
{
    printf(">>> NET:\r\n");
    printf("    IP:      %u.%u.%u.%u\r\n", IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3]);
    printf("    NETMASK: %u.%u.%u.%u\r\n", NETMASK_ADDRESS[0], NETMASK_ADDRESS[1], NETMASK_ADDRESS[2], NETMASK_ADDRESS[3]);
    printf("    GATEWAY: %u.%u.%u.%u\r\n", GATEWAY_ADDRESS[0], GATEWAY_ADDRESS[1], GATEWAY_ADDRESS[2], GATEWAY_ADDRESS[3]);
    printf("    MAC:     %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
           gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);
    printf("\r\n");
}
*/

	static uint8_t Run_App_Update(AppArea_t App){
			FlashParam_t param;
      if (Param_Load(&param) != 0U) {
          printf("    Load param failed...\r\n");
          return 1U;
      }
			param.boot_select = App;
      if (Param_Save(&param) != 0U){
					printf("    Save param failed...\r\n");
          return 1U;
			}
      return Boot_CopyRunAndJump(&param, "    Copy to RunApp failed...\r\n");
	}

  static void Eth_OnBytes(const uint8_t *data, uint16_t len)
  {
    if (data == NULL || len == 0U) {
      return;
    }

    for (uint16_t i = 0; i < len; i++) {
      uint8_t result = Protocol_ReceiveByte(&g_eth_protocol_rx_buf, data[i]);

      if (result == 1U) {
        ProtocolFrame_t frame;
        if (Protocol_Unpack(g_eth_protocol_rx_buf.buffer, g_eth_protocol_rx_buf.index, &frame) == 0U) {
          Protocol_HandleFrame(&frame, by_eth);
        }
        Protocol_InitRxBuffer(&g_eth_protocol_rx_buf);
      } else if (result == 2U) {
        Protocol_InitRxBuffer(&g_eth_protocol_rx_buf);
      }
    }
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
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_CRC_Init();
  MX_LWIP_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
    //Wifi模块初始化
    printf(">>> WiFi Initialization...\r\n");
	Wifi_Init();
    printf(">>> WiFi Done\r\n");

    Protocol_InitRxBuffer(&g_eth_protocol_rx_buf);
    EthTcpServer_Init(5000, Eth_OnBytes);
    s_eth_prev_connected = EthTcpServer_IsConnected();
    // Print_NetInfo();
    printf("\r\n\r\n");
    printf("=========================================\r\n");
    printf("=      STM32F407 IAP/OTA Bootloader     =\r\n");
    printf("=========================================\r\n");
    printf("\r\n");
	
    // 初始化协议处理模块
    Protocol_Handler_Init();
    printf("\r\n");
    
	// 获取参数区参数
    FlashParam_t param;
    // 加载参数
    if (Param_Load(&param) != 0U) {
	        printf("Start param init \r\n");
		    Param_Init(&param);
			printf("Start param save \r\n");
			Param_Save(&param);
		}
		
    // 启动串口接收中断
    printf(">>> Starting UART receive interrupt...\r\n");
    HAL_UART_Receive_IT(&huart1, &g_uart1_rx_byte, 1);
    HAL_UART_Receive_IT(&huart3, &g_uart3_rx_byte, 1);
    printf(">>> UART interrupt enabled\r\n");
    printf("\r\n");

    printf(">>> Waiting For CMD...\r\n");		
    int tick = HAL_GetTick();
    while((HAL_GetTick() - tick) < 10000) {
        if (g_stay_in_bootloader) {
          printf("    Stay in Bootloader permanent wait mode\r\n");
          break;
        }
        MX_LWIP_Process();
        EthTcpServer_Poll();
    }
		
    if (!g_stay_in_bootloader) {
			printf("    Jump to RunApp Start...\r\n");
				
        if (((param.app_a_status == APP_STATUS_VALID) && (param.app_a_version > param.run_app_version) ))
        {
			Run_App_Update(APP_AREA_A);
        }
				else if((param.app_b_status == APP_STATUS_VALID) && (param.app_b_version > param.run_app_version)){
				Run_App_Update(APP_AREA_B);
				}

        if (Param_Load(&param) != 0U) {
            printf("    Reload param failed...\r\n");
        }
        
        if (param.run_app_status == APP_STATUS_VALID) {
            if (Boot_IsRunAppValid() == 0U) {
            	printf("    run_app is runnable , ready to jump to runapp...\r\n");
                Boot_JumpToRunApp();
            } else {
                param.run_app_status = APP_STATUS_INVALID;
					if (Param_Save(&param) != 0U) {
    					printf("    Save param failed...\r\n");
                }
                printf("    RunApp invalid, check if app a or b is valid...\r\n");
            }
        }
				// 若未选择App区域 或 RunApp状态无效 则尝试复制选中的App到RunApp
				else if (param.boot_select == APP_AREA_NONE || param.run_app_status == APP_STATUS_INVALID) {  
            printf("    Boot select none/run invalid, start to copy to RunApp...\r\n");
            Boot_CopyRunAndJump(&param, "    Copy to RunApp failed, stay in Bootloader...\r\n");
        }
    }
		
    if (g_stay_in_bootloader) {
        printf("    Stay in Bootloader permanent wait mode\r\n");
    }
    
	printf("timeout");
    
	
	
    
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    MX_LWIP_Process();
    EthTcpServer_Poll();
    uint8_t eth_now_connected = EthTcpServer_IsConnected();
    if ((s_eth_prev_connected == 1U) && (eth_now_connected == 0U)) {
      Protocol_InitRxBuffer(&g_eth_protocol_rx_buf);
      Protocol_Handler_OnTransportClosed(by_eth);
      printf("[ETH] client disconnected, rx state reset\r\n");
    }
    s_eth_prev_connected = eth_now_connected;
		
    if (g_update_finish){
		Param_Load(&param);
		if (param.run_app_status == APP_STATUS_VALID){
	  		Boot_JumpToRunApp();
	  	}
        else if (Boot_CopySelectedAppToRun() == 0U){
			Boot_JumpToRunApp();
		}
	}
    
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

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
  if (htim->Instance == TIM1)
  {
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
#ifdef USE_FULL_ASSERT
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
