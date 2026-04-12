#ifndef __PROTOCOL_HANDLER_H
#define __PROTOCOL_HANDLER_H

#include "protocol.h"
#include <stdint.h>

typedef void (*CmdHandler_t)(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

typedef struct{
	uint16_t cmd;
	CmdHandler_t handler;       // 处理函数
	const char *description;    // 命令描述（调试用）
}CmdHandlerEntry_t;

extern ProtocolRxBuffer_t g_protocol_uart1_rx_buf;
extern ProtocolRxBuffer_t g_protocol_uart3_rx_buf;

/* ==================== 函数声明 ==================== */

/**
 * @brief  初始化协议处理模块
 */
void Protocol_Handler_Init(void);

/**
 * @brief  处理接收到的协议帧
 * @param  rx_frame: 接收到的帧
 */
void Protocol_HandleFrame(ProtocolFrame_t *rx_frame, UpdateMethod_t source);

/**
 * @brief  发送响应帧
 * @param  frame: 要发送的帧
 */
void Protocol_SendResponse(ProtocolFrame_t *frame, UpdateMethod_t source);

/* ==================== 具体命令处理函数 ==================== */

/**
 * @brief  心跳命令处理
 */
void CMD_Handler_Ping(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

/**
 * @brief  查询版本命令处理
 */
void CMD_Handler_GetVersion(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

/**
 * @brief  复位命令处理
 */
void CMD_Handler_Reset(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

/**
 * @brief  读取参数命令处理
 */
void CMD_Handler_ParamRead(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

/**
 * @brief  写入参数命令处理
 */
void CMD_Handler_ParamWrite(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

void CMD_Handler_UpdateStart(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);
void CMD_Handler_UpdateData(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);
void CMD_Handler_UpdateEnd(ProtocolFrame_t *rx_frame, ProtocolFrame_t *tx_frame);

void Protocol_Handler_OnTransportClosed(UpdateMethod_t source);

#endif
