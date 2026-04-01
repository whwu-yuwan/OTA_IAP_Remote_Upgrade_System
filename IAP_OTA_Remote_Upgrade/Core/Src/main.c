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

extern uint8_t g_uart_rx_byte;
volatile uint8_t g_stay_in_bootloader = 0U;
volatile uint8_t g_update_finish = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint8_t Boot_CopySelectedAppToRun(void);
static uint8_t Boot_IsRunAppValid(void);
static void Boot_JumpToRunApp(void);
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
  MX_USART1_UART_Init();
  MX_CRC_Init();
  MX_LWIP_Init();
  /* USER CODE BEGIN 2 */
    HAL_Delay(100);
    
    printf("\r\n\r\n");
    printf("=========================================\r\n");
    printf("=      STM32F407 IAP/OTA Bootloader     =\r\n");
    printf("=========================================\r\n");
    printf("\r\n");
    
    // 打印系统信息
    printf(">>> System Information:\r\n");
    printf("    MCU:       STM32F407ZGT6\r\n");
    printf("    SYSCLK:    %du MHz\r\n", HAL_RCC_GetSysClockFreq() / 1000000);
    printf("    Build:     %s %s\r\n", __DATE__, __TIME__);
    printf("\r\n");
    
    // 初始化协议处理模块
    Protocol_Handler_Init();
    printf("\r\n");
    
    // 打印Flash分区信息
    Flash_PrintPartitionInfo();
    
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
    HAL_UART_Receive_IT(&huart1, &g_uart_rx_byte, 1);
    printf(">>> UART interrupt enabled\r\n");
    printf("\r\n");

    printf(">>> Protocol Test Started...\r\n");
    for (int i = 0; i < 10; i++) {
        printf("    Test %d...\r\n", i + 1);
        if (g_stay_in_bootloader) {
          printf("    Stay in Bootloader permanent wait mode\r\n");
          break;
        }
        HAL_Delay(1000);
    }
		
    
		
		
    if (!g_stay_in_bootloader) {
				printf("    Jump to RunApp Start...\r\n");
				// 先检查看是否选择了App区域 若选择了则检查RunApp是否有效
        if (param.run_app_status == APP_STATUS_VALID) {
            if (Boot_IsRunAppValid() == 0U) {
								printf("    run_app is runnable , ready to jump to runapp...\r\n");
                Boot_JumpToRunApp();
            } else {
                // RunApp无效 则将参数中的run_app_status设置为无效
                param.run_app_status = APP_STATUS_INVALID;
								if (Param_Save(&param) != 0U) {
										printf("    Save param failed...\r\n");
                }
                printf("    RunApp invalid, check if app a or b is valid...\r\n");
            }
        }
				// 若未选择App区域 或 RunApp状态无效 则尝试复制选中的App到RunApp
				else if (param.boot_select == APP_AREA_NONE || param.run_app_status == APP_STATUS_INVALID) {  
            printf("    Boot select none, start to copy to RunApp...\r\n");
            if (Boot_CopySelectedAppToRun() == 0U) {
                printf("    Copy to RunApp success...\r\n");
                if (Boot_IsRunAppValid() == 0U) {
                    Boot_JumpToRunApp();
                } 
								else {
                    param.run_app_status = APP_STATUS_INVALID;
                    if (Param_Save(&param) != 0U) {
                        printf("    Save param failed...\r\n");
                    }
                    printf("    RunApp invalid/CRC failed/Vector invalid...\r\n");
                }
            }
						else {
                printf("    Copy to RunApp failed, stay in Bootloader...\r\n");
            }
        }
    }
		
    if (g_stay_in_bootloader) {
        printf("    Stay in Bootloader permanent wait mode\r\n");
    }
    

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (g_update_finish){
				Param_Load(&param);
				if (param.run_app_status == APP_STATUS_VALID){
					Boot_JumpToRunApp();
				}
				else if (Boot_CopySelectedAppToRun()){
					Boot_JumpToRunApp();
				}
		}
	  HAL_Delay(5000);
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
