#ifndef __ETH_TCP_SERVER_H
#define __ETH_TCP_SERVER_H

#include <stdint.h>

typedef void (*EthTcpOnBytes_t)(const uint8_t *data, uint16_t len);
// 初始化TCP服务器
void EthTcpServer_Init(uint16_t listen_port, EthTcpOnBytes_t on_bytes);
// 反初始化TCP服务器
void EthTcpServer_DeInit(void);
// 轮询TCP服务器
void EthTcpServer_Poll(void);
// 检查TCP服务器是否已连接
uint8_t EthTcpServer_IsConnected(void);
// 发送数据到TCP服务器  
uint8_t EthTcpServer_Send(const uint8_t *data, uint16_t len);

#endif
