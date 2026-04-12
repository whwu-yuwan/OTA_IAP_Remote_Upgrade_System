#include "wifi_at.h"
#include <stdio.h>
#include "usart.h"
#include <string.h>

#define WIFI_AT_REPLY_TIMEOUT_MS 5000
#define WIFI_AT_JOIN_TIMEOUT_MS 20000 //AP连接时间
#define WIFI_AT_TCP_TIMEOUT_MS 40000 //TCP连接时间

static uint8_t Wifi_WaitForReplyTimeout(uint32_t timeout_ms , char* expected_reply){
	uint32_t start_tick = HAL_GetTick();
	uint8_t received_byte;
	char reply_buffer[256];
	uint16_t reply_length = 0;

	memset(reply_buffer, 0, sizeof(reply_buffer));

	while (HAL_GetTick() - start_tick < timeout_ms)
	{
		if (HAL_UART_Receive(&huart3, &received_byte, 1, 50) != HAL_OK) {
			continue;
		}

		if (reply_length < sizeof(reply_buffer) - 1) {
			reply_buffer[reply_length++] = (char)received_byte;
			reply_buffer[reply_length] = '\0';
		}

		if (strstr(reply_buffer, expected_reply) != NULL) {
			return WIFI_OK; // Received "OK"
		}

		if ((strstr(reply_buffer, "ERROR") != NULL) ||
			(strstr(reply_buffer, "FAIL") != NULL)) {
			return WIFI_ERROR; // Received "ERROR" or "FAIL"
		}
	}

	printf("Received: %s\n", reply_buffer);
	return WIFI_ERROR;
}

static uint8_t Wifi_SendMessageToESPAndWait(char* cmd, char* expected_reply){
	HAL_UART_Transmit(&huart3, (uint8_t *)cmd, strlen(cmd), 1000);
	HAL_UART_Transmit(&huart3, (uint8_t *)"\r\n", 2, 1000);
	return Wifi_WaitForReplyTimeout(WIFI_AT_REPLY_TIMEOUT_MS, expected_reply);
}

static uint8_t Wifi_SendMessageToESPAndWaitTimeout(char* cmd, uint32_t timeout_ms , char* expected_reply){
	HAL_UART_Transmit(&huart3, (uint8_t *)cmd, strlen(cmd), 1000);
	HAL_UART_Transmit(&huart3, (uint8_t *)"\r\n", 2, 1000);
	return Wifi_WaitForReplyTimeout(timeout_ms, expected_reply);
}

uint8_t Wifi_ConnectToAP(void){
	HAL_Delay(2000);
	if (Wifi_SendMessageToESPAndWait(AT_TEST, "OK") != WIFI_OK)
	{
		printf("Failed to connect to AP at AT_TEST\n");
		return WIFI_ERROR;
	}
	
	if (Wifi_SendMessageToESPAndWait(AT_RESET, "OK") != WIFI_OK)
	{
		printf("Failed to connect to AP at AT_RESET\n");
		return WIFI_ERROR;
	}
	HAL_Delay(2000); // Wait for ESP to reset

	if (Wifi_SendMessageToESPAndWait(AT_CWMODE_3, "OK") != WIFI_OK)
	{
		printf("Failed to connect to AP at AT_CWMODE_3\n");
		return WIFI_ERROR;
	}

	if (Wifi_SendMessageToESPAndWaitTimeout(AT_CONNECT_AP, WIFI_AT_JOIN_TIMEOUT_MS, "OK") != WIFI_OK)
	{
		printf("Failed to connect to AP at AT_CONNECT_AP\n");
		return WIFI_ERROR;
	}
	printf("Successfully connected to AP\n");
	return WIFI_OK;
}

uint8_t Wifi_TCPClient_Init(void){
	for (uint8_t i = 0u ; i < 5 ; i ++){
		if (Wifi_SendMessageToESPAndWaitTimeout(AT_CIPSTART, WIFI_AT_TCP_TIMEOUT_MS, "OK") != WIFI_OK)
		{
			printf("Failed to initialize TCP client at AT_CIPSTART\n");
			return WIFI_ERROR;
		}
		if (Wifi_SendMessageToESPAndWaitTimeout(AT_CIPMODE_1, WIFI_AT_REPLY_TIMEOUT_MS, "OK") != WIFI_OK)
		{
			printf("Failed to initialize TCP client at AT_CIPMODE_1\n");
			return WIFI_ERROR;
		}
		printf("TCP client initialized successfully\n");
		return WIFI_OK;
	}	
}

void Wifi_Init(void){
	if (Wifi_ConnectToAP() != WIFI_OK){
		return;
	}
	if (Wifi_TCPClient_Init() != WIFI_OK){
		return;
	}
	if (ESP8266_StartTransparentTransmission() != WIFI_OK){
		return;
	}
	if (Wifi_SendDataToESP((const uint8_t*)"Hello from STM32!") != WIFI_OK){
		return;
	}
}

uint8_t ESP8266_StartTransparentTransmission(void){
	if (Wifi_SendMessageToESPAndWaitTimeout(AT_CIPMODE_1, WIFI_AT_JOIN_TIMEOUT_MS, "OK") != WIFI_OK)
	{
		printf("Failed to start transparent transmission at AT_CIPMODE_1\n");
		return WIFI_ERROR;
	}
	if (Wifi_SendMessageToESPAndWait(AT_CIPSEND, ">") != WIFI_OK)
	{
		printf("Failed to start transparent transmission at AT_CIPSEND\n");
		return WIFI_ERROR;
	}
	printf("Transparent transmission mode started successfully\n");
	return WIFI_OK;
}

uint8_t Wifi_SendDataToESP(const uint8_t* data){
	HAL_UART_Transmit(&huart3, data, strlen((char*)data), 1000);
	return WIFI_OK;
}

uint8_t ESP8266_EndTransparentTransmission(void){
	Wifi_SendDataToESP((const uint8_t*)"+++");
	HAL_Delay(1000); // Wait for a second
	if (Wifi_SendMessageToESPAndWait(AT_CIPMODE_0, "OK") != WIFI_OK)
	{
		printf("Failed to end transparent transmission at AT_CIPMODE_0\n");
		return WIFI_ERROR;
	}
	printf("Transparent transmission mode ended successfully\n");
	return WIFI_OK;
}


