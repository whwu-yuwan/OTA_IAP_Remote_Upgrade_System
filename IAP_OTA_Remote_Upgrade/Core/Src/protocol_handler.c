#include <stdio.h>
#include <string.h>
#include "flash_manage.h"
#include "protocol.h"
#include "protocol_handler.h"
#include "usart.h"
#include "eth_tcp_server.h"

// 协议接收缓冲区（全局，供中断使用）
ProtocolRxBuffer_t g_protocol_uart1_rx_buf;
ProtocolRxBuffer_t g_protocol_uart3_rx_buf;
extern volatile uint8_t g_stay_in_bootloader;
extern volatile UpdateMethod_t g_update_method;

static uint8_t g_update_active = 0;          // 0=未升级 1=升级中
static AppArea_t g_update_target = APP_AREA_NONE; // 当前目标区A/B
static uint32_t g_update_addr = 0;           // 当前写入地址
static uint32_t g_update_total = 0;          // 固件总长度
static uint32_t g_update_recv = 0;           // 已接收长度
static uint8_t g_uart_log_defer_enable = 0U;
static UpdateMethod_t g_current_source = no_update;
static UpdateMethod_t g_update_source = no_update;

static void UpdateSessionReset(void)
{
	g_update_active = 0U;
	g_update_source = no_update;
	g_update_target = APP_AREA_NONE;
	g_update_addr = 0U;
	g_update_total = 0U;
	g_update_recv = 0U;
	g_update_method = no_update;
}
// 传输通道关闭回调（如TCP断开），如果正在升级且来源匹配则重置升级状态
void Protocol_Handler_OnTransportClosed(UpdateMethod_t source)
{
	if (source == no_update) {
		return;
	}

	if (g_update_active == 1U && g_update_source == source) {
		UpdateSessionReset();
		UART_LogEnable(1U);
		g_uart_log_defer_enable = 0U;
	}
}
/* ==================== 命令处理注册表 ==================== */

const CmdHandlerEntry_t g_cmd_handler_table[] = {
    // 命令码              处理函数                   描述
    {CMD_PING,            CMD_Handler_Ping,          "心跳/Ping"},
    {CMD_GET_VERSION,     CMD_Handler_GetVersion,    "获取版本"},
    {CMD_RESET,           CMD_Handler_Reset,         "系统复位"},
    {CMD_PARAM_READ,      CMD_Handler_ParamRead,     "读取参数"},
    {CMD_PARAM_WRITE,     CMD_Handler_ParamWrite,    "写入参数"},
	  {CMD_UPDATE_START,    CMD_Handler_UpdateStart,   "StartUpdate"},
    {CMD_UPDATE_DATA,     CMD_Handler_UpdateData,    "UpdateData"},
    {CMD_UPDATE_END,      CMD_Handler_UpdateEnd,     "UpdateEnd"},
};

#define CMD_HANDLER_COUNT  (sizeof(g_cmd_handler_table) / sizeof(CmdHandlerEntry_t))

//初始化协议模块
void Protocol_Handler_Init(void){
	Protocol_InitRxBuffer(&g_protocol_uart1_rx_buf);  
	Protocol_InitRxBuffer(&g_protocol_uart3_rx_buf);
	/*
	printf("[Protocol] Handler initialized\r\n");
    printf("[Protocol] Registered %d commands:\r\n", CMD_HANDLER_COUNT);
    
    for (int i = 0; i < CMD_HANDLER_COUNT; i++) {
       printf("  - 0x%04X: %s\r\n", g_cmd_handler_table[i].cmd,g_cmd_handler_table[i].description);
    }
	*/
}

void Protocol_HandleFrame(ProtocolFrame_t *rx_frame, UpdateMethod_t source){
	ProtocolFrame_t tx_frame;
	uint8_t handle = 0;
	g_stay_in_bootloader = 1;
	g_current_source = source;
	
	for(int i = 0 ; i < CMD_HANDLER_COUNT ; i ++){
		if (rx_frame->cmd == g_cmd_handler_table[i].cmd){
			g_cmd_handler_table[i].handler(rx_frame, &tx_frame);
			Protocol_SendResponse(&tx_frame, source);
			handle = 1;
			break;
		}
	}
	
	if (!handle){
		Protocol_CreateResponse(&tx_frame, CMD_NACK, NULL, 0);
		Protocol_SendResponse(&tx_frame, source);
	}

	if (g_uart_log_defer_enable != 0U) {
		UART_LogEnable(1U);
		g_uart_log_defer_enable = 0U;
	}
}

void Protocol_SendResponse(ProtocolFrame_t *frame, UpdateMethod_t source){
	uint8_t tx_buffer[300];
	uint16_t len;
	extern UART_HandleTypeDef huart1;
	
	len = Protocol_Pack(frame, tx_buffer, sizeof(tx_buffer));
	
	if (len > 0){
		/* 根据通道来源选择发送方式 */
		if (source == by_eth) {
			/* ETH 通道：通过 TCP 发送 */
			if (EthTcpServer_Send(tx_buffer, len) == 0U) {
				printf("[protocol] ETH send data: 0x%04X (%d bytes)\r\n", frame->cmd, len);
			} else {
				printf("[protocol] ETH send failed\r\n");
			}
		} 
		else if (source == by_wifi) {
			/* WiFi 通道：通过 ESP8266 发送 */
			if (Wifi_SendDataToESP_Len(tx_buffer, len) == 0U) {
				printf("[protocol] WiFi send data: 0x%04X (%d bytes)\r\n", frame->cmd, len);
			} else {
				printf("[protocol] WiFi send failed\r\n");
			}
		}
		else {
			/* UART 或其他通道：通过串口发送 */
			HAL_UART_Transmit(&huart1, tx_buffer, len, 1000);
			printf("[protocol] UART send data: 0x%04X (%d bytes)\r\n", frame->cmd, len);
		}
	}
	else {
		printf("[protocol] ProtocolPack failed\r\n");
	}
}

/* ==================== 具体命令处理函数 ==================== */
void CMD_Handler_Ping(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	printf("[CMD] Ping Received\r\n");
	
	Protocol_CreateResponse(tx_frame, CMD_PING, rx_frame->data, rx_frame->length);
}

void CMD_Handler_GetVersion(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
    // 版本信息结构体
    typedef struct {
        uint32_t boot_version;      // Boot版本
        uint32_t app_version;       // App版本
        uint32_t hw_version;        // 硬件版本
        char     build_date[12];    // 编译日期
    } __attribute__((packed)) VersionInfo_t;
    
    VersionInfo_t version_info;
    
    // 填充版本信息
    version_info.boot_version = 0x00010000;  // V1.0.0
    version_info.app_version = 0x00020100;   // V2.1.0
    version_info.hw_version = 0x00010000;    // V1.0.0
    strncpy(version_info.build_date, __DATE__, 11);
    version_info.build_date[11] = '\0';
    
    printf("[CMD] Get Version\r\n");
    printf("  Boot: V%du.%du.%du\r\n",
           (version_info.boot_version >> 16) & 0xFF,
           (version_info.boot_version >> 8) & 0xFF,
           version_info.boot_version & 0xFF);
    printf("  App:  V%du.%du.%du\r\n",
           (version_info.app_version >> 16) & 0xFF,
           (version_info.app_version >> 8) & 0xFF,
           version_info.app_version & 0xFF);
    printf("  Build: %s\r\n", version_info.build_date);
    
    // 返回版本信息
    Protocol_CreateResponse(tx_frame, CMD_ACK, 
                           (uint8_t *)&version_info, 
                           sizeof(VersionInfo_t));
}

void CMD_Handler_Reset(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	printf("[CMD] reset command receive\r\n");
	
	Protocol_CreateResponse(tx_frame, CMD_ACK, NULL, 0);
	
	HAL_Delay(1000);
	NVIC_SystemReset();
}

void CMD_Handler_ParamRead(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	FlashParam_t param;
	uint8_t result;
	
	result = Param_Load(&param);
	if (result == 0){
		typedef struct {
		uint32_t boot_version;
        uint32_t boot_run_count; 
		uint32_t run_app_version;
        uint8_t  app_a_status;
        uint8_t  app_b_status;
		} __attribute__((packed)) ParamSummary_t;
		
		ParamSummary_t ps;
		ps.boot_version = param.boot_version;
		ps.boot_run_count = param.boot_run_count;
		ps.app_a_status = param.app_a_status;
		ps.app_b_status = param.app_b_status;
		ps.run_app_version = param.run_app_version;
		
		Protocol_CreateResponse(tx_frame, CMD_ACK, (uint8_t *)&ps, sizeof(ps));
	}
	else{
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
	}
}

void CMD_Handler_ParamWrite(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	printf("[CMD] Write parameters\r\n");
    
    FlashParam_t param;
    uint8_t result;
    
    result = Param_Load(&param);
    if (result == 0) {
        param.boot_run_count++;
        result = Param_Save(&param);
        
        if (result == 0) {
            Protocol_CreateResponse(tx_frame, CMD_ACK, NULL, 0);
        } else {
            Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
        }
    } else {
        Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
    }
}

void CMD_Handler_UpdateStart(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	typedef struct{
		uint32_t target;
		uint32_t version;
		uint32_t size;
		uint32_t crc32;
	}__attribute__((packed))UpdateStartInfo_t;
	UpdateStartInfo_t info;
	FlashParam_t param;
	uint32_t start_addr;
	uint32_t end_addr;

	if (g_update_active == 1U) {
		Protocol_CreateResponse(tx_frame, CMD_BUSY, NULL, 0);
		return;
	}

	if (g_update_source != no_update && g_update_source != g_current_source) {
		Protocol_CreateResponse(tx_frame, CMD_BUSY, NULL, 0);
		return;
	}

	if (rx_frame->length < sizeof(UpdateStartInfo_t)){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	memcpy(&info, rx_frame->data, sizeof(info));
	
	if (info.target != APP_AREA_A && info.target != APP_AREA_B){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	if ((info.size > APP_A_SECTOR_SIZE) || (info.size == 0) || ((info.size % 4) != 0)){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}

	g_update_source = g_current_source;
	g_update_method = g_current_source;
	
	g_update_active = 1;          // 0=未升级 1=升级中
	g_update_target = info.target; // 当前目标区A/B
	g_update_addr = (info.target == APP_AREA_A) ? APP_A_SECTOR_START_ADDR : APP_B_SECTOR_START_ADDR;		// 当前写入地址
	g_update_total = info.size;          // 固件总长度
	g_update_recv = 0;           // 已接收长度
	UART_LogEnable(0U);
	
    if (info.target == APP_AREA_A){
		start_addr = Flash_GetSector(APP_A_SECTOR_START_ADDR);
		end_addr  = Flash_GetSector(APP_A_SECTOR_START_ADDR + info.size - 1U);
	}
	else{
		start_addr = Flash_GetSector(APP_B_SECTOR_START_ADDR);
		end_addr  = Flash_GetSector(APP_B_SECTOR_START_ADDR + info.size - 1U);
	}
	
	if (Flash_EraseSector(start_addr, end_addr) != 0U){
		UpdateSessionReset();
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	
	if (Param_Load(&param) != 0){
		Param_Init(&param);
	}
	
	if (info.target == APP_AREA_A){
		param.app_a_status = APP_STATUS_UPDATING;
		param.app_a_version = info.version;
		param.app_a_crc32 = info.crc32;
		param.app_a_size = info.size;
		param.update_target = APP_AREA_A;
		
	}
	else {
		param.app_b_status = APP_STATUS_UPDATING;
		param.app_b_version = info.version;
		param.app_b_crc32 = info.crc32;
		param.app_b_size = info.size;
		param.update_target = APP_AREA_B;
	}
	
	Param_Save(&param);
	Protocol_CreateResponse(tx_frame, CMD_ACK, NULL, 0);
}

void CMD_Handler_UpdateData(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	if (g_update_source != g_current_source)
	{
		Protocol_CreateResponse(tx_frame, CMD_BUSY, NULL, 0);
		return;
	}
	
	//判断更新在进行
	if (g_update_active != 1){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	//数据不能为零,且能被四整除
	if(rx_frame->length == 0 || (rx_frame->length % 4) != 0){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	//总长度不能超过更新长度
	if ((g_update_recv + rx_frame->length) > g_update_total) {
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	
	if (Flash_Write(g_update_addr, rx_frame->data, rx_frame->length) != 0){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	
	g_update_addr += rx_frame->length;
	g_update_recv += rx_frame->length;
	Protocol_CreateResponse(tx_frame, CMD_ACK, NULL, 0);
}

void CMD_Handler_UpdateEnd(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame){
	if (g_update_source != g_current_source)
	{
		Protocol_CreateResponse(tx_frame, CMD_BUSY, NULL, 0);
		return;
	}
	
	extern CRC_HandleTypeDef hcrc;
	FlashParam_t param;
	uint32_t crc;
	g_uart_log_defer_enable = 1U;
	if (Param_Load(&param)){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	
	if (g_update_active != 1 || (g_update_recv != g_update_total)){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}

	if (param.update_target == APP_AREA_A){
		__HAL_CRC_DR_RESET(&hcrc);
		crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)APP_A_SECTOR_START_ADDR, g_update_total / 4U);
		if (crc != param.app_a_crc32){
			Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
			param.app_a_status = APP_STATUS_INVALID;
            Param_Save(&param);
			UpdateSessionReset();
			return;
		}
		param.app_a_status = APP_STATUS_VALID;
		param.boot_select = APP_AREA_A;
	}
	else if (param.update_target == APP_AREA_B){
		__HAL_CRC_DR_RESET(&hcrc);
		crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)APP_B_SECTOR_START_ADDR, g_update_total / 4U);
		if (crc != param.app_b_crc32){
			Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
			param.app_b_status = APP_STATUS_INVALID;
            Param_Save(&param);
			UpdateSessionReset();
			return;
		}
		param.app_b_status = APP_STATUS_VALID;
		param.boot_select = APP_AREA_B;
	}
    else{
        Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
        return;
    }
	
	param.update_target = APP_AREA_NONE;
	if (Param_Save(&param) != 0U){
		Protocol_CreateResponse(tx_frame, CMD_NACK, NULL, 0);
		return;
	}
	UpdateSessionReset();
	Protocol_CreateResponse(tx_frame, CMD_ACK, NULL, 0);
}

