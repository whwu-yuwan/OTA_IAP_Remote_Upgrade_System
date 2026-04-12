#ifndef __WIFI_AT_H
#define __WIFI_AT_H

#include <stdint.h>

#define WIFI_OK 0
#define WIFI_ERROR 1

#define WIFI_AP_SSID "muyou"
#define WIFI_AP_PASSWORD "wwh664311"
#define TCP_IP  "10.140.62.154"
#define TCP_PORT "8080"

#define AT_TEST "AT"
#define AT_RESET "AT+RST"
#define AT_CWMODE_3 "AT+CWMODE=3"
#define AT_CONNECT_AP "AT+CWJAP=\"" WIFI_AP_SSID "\",\"" WIFI_AP_PASSWORD "\""
#define AT_CIPSTART "AT+CIPSTART=\"TCP\",\"" TCP_IP "\"," TCP_PORT
#define AT_CIPMODE_1 "AT+CIPMODE=1" //开启透传
#define AT_CIPSEND "AT+CIPSEND" //想服务器发数据
#define AT_CIPMODE_0 "AT+CIPMODE=0" //关闭透传



uint8_t Wifi_ConnectToAP(void);
uint8_t Wifi_TCPClient_Init(void);
void Wifi_Init(void);
uint8_t Wifi_SendDataToESP(const uint8_t* data);
uint8_t Wifi_SendDataToESP_Len(const uint8_t* data, uint16_t len);
uint8_t ESP8266_EndTransparentTransmission(void);
uint8_t ESP8266_StartTransparentTransmission(void);

#endif
