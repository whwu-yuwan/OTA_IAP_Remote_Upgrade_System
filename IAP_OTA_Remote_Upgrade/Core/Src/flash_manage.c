#include "flash_manage.h"
#include <stdio.h>
#include <string.h>

extern CRC_HandleTypeDef hcrc;

const FlashSectorInfo_t g_flash_sector_table[12] = {
	{FLASH_SECTOR_0 ,  0x08000000 , 16*1024},
	{FLASH_SECTOR_1 ,  0x08004000 , 16*1024},
	{FLASH_SECTOR_2 ,  0x08008000 , 16*1024},
	{FLASH_SECTOR_3 ,  0x0800C000 , 16*1024},
	{FLASH_SECTOR_4 ,  0x08010000 , 64*1024},
	{FLASH_SECTOR_5 ,  0x08020000 , 128*1024},
	{FLASH_SECTOR_6 ,  0x08040000 , 128*1024},
	{FLASH_SECTOR_7 ,  0x08060000 , 128*1024},
	{FLASH_SECTOR_8 ,  0x08080000 , 128*1024},
	{FLASH_SECTOR_9 ,  0x080A0000 , 128*1024},
	{FLASH_SECTOR_10 , 0x080C0000 , 128*1024},
	{FLASH_SECTOR_11 , 0x080E0000 , 128*1024}
};

// 修改CalcCRC32函数
static uint32_t CalcCRC32(uint32_t *data, uint32_t len)
{
    __HAL_CRC_DR_RESET(&hcrc);
    return HAL_CRC_Calculate(&hcrc, data, len);
}

//获取地址所处的扇区: 输入地址(addr), 返回对应的所处扇区
uint32_t Flash_GetSector(uint32_t addr){
	for (int i  = 0 ; i <= 11 ; i ++){
		if (addr >= g_flash_sector_table[i].start_addr && addr <= (g_flash_sector_table[i].start_addr + g_flash_sector_table[i].size - 1) ){
			//printf("[Flash]经查找得到addr: %u  所处扇区为 Flash_Sector_%u \r\n" , addr , i);
			return g_flash_sector_table[i].sector_num;
		}
	}
	//printf("[Flash]查找失败, 返回默认扇区Flash_Sector_11");
	return g_flash_sector_table[11].sector_num; //默认返回扇区11
}

//擦除指定扇区的地址 (0为成功 , 1为失败)
uint8_t Flash_EraseSector(uint32_t Sector_Start , uint32_t Sector_End){
	HAL_StatusTypeDef status;
	FLASH_EraseInitTypeDef erase_init;
	uint32_t sector_error;
	
	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	
	erase_init.Sector = Sector_Start;
	erase_init.NbSectors = Sector_End - Sector_Start + 1; 
	erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
	
	status = HAL_FLASHEx_Erase(&erase_init, &sector_error);
	
	HAL_FLASH_Lock();
	
	if (status != HAL_OK) {
		//printf("[Flash] Erase failed! Error: 0x%u\r\n", sector_error);
		return 1;
    }
	//printf("[Flash] Erase success!\r\n");
	return 0;
}

// 写数据到Flash（addr必须是4字节对齐，len以字节为单位）
uint8_t Flash_Write(uint32_t addr, uint8_t *data, uint32_t len){
	HAL_StatusTypeDef status;
	uint32_t write_addr = addr;
	uint32_t addr_end = addr + len;
	uint32_t *p_data =  (uint32_t *)data;
	
	if ((addr % 4) != 0){
		//printf("[Flash] Address not aligned: 0x%u\r\n", addr);
		return 1;
	}
	
	HAL_FLASH_Unlock();
	
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	while(write_addr < addr_end){
		status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, write_addr, *p_data);
		if (status != HAL_OK){
			//printf("[Flash] Write failed at 0x%u\r\n", write_addr);
			return 1;
		}
		write_addr += 4;
		p_data += 1;
	}
	
	HAL_FLASH_Lock();
	//printf("[Flash] write words success!\r\n");
	return 0;
}

// 从Flash读数据（len以字节为单位）
void Flash_Read(uint32_t addr, uint8_t *data, uint32_t len){
	for (uint32_t i = 0 ; i < len ; i ++){
		data[i] = *((uint8_t *)(addr)+ i);
	}
	//printf("[Flash] Read Success");
}

// 写入一个32位字
uint8_t Flash_WriteWord(uint32_t addr, uint32_t data){
	HAL_StatusTypeDef status;
	
	if (addr % 4 != 0) {
		return 1;
    }
	
	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);
	HAL_FLASH_Lock();
	
	return (status == HAL_OK) ?0:1;
}

// 读取一个32位字
uint32_t Flash_ReadWord(uint32_t addr){
	return *(volatile uint32_t *)addr; 

}

/* ==================== 参数区操作 ==================== */

uint8_t Param_Save(FlashParam_t *param){
	uint8_t ret;
	param->crc32 = CalcCRC32((uint32_t *)param, (sizeof(FlashParam_t) - 4U)/4U);
	
	ret = Flash_EraseSector(PARAM_SECTOR_START, PARAM_SECTOR_END);
	if (ret == 1){
		//printf("[Flash] Param_Save Failed at Flash_Erase\r\n");
		return 1;
	}

	ret = Flash_Write(PARAM_SECTOR_START_ADDR, (uint8_t *)param, sizeof(FlashParam_t));
	if (ret == 1){
		//printf("[Flash] Param_Save Failed at Flash_Write\r\n");
		return 1;
	}
	//printf("[Param] Param_Save success!\r\n");
	return 0;
}

//读取并计算crc成功返回0
uint8_t Param_Load(FlashParam_t *param){
	uint32_t crc32;
	Flash_Read(PARAM_SECTOR_START_ADDR, (uint8_t *)param, sizeof(FlashParam_t));
	crc32 = CalcCRC32((uint32_t *)param, (sizeof(FlashParam_t) - 4U)/4U);
	if ((param->valid_flag == FLAG_VALID) && (param->crc32 == crc32)){
		//printf("[param] param load success\r\n");
		return 0;
	}
	else{
		//printf("[param] param load fail\r\n");
		return 1;
	}
	
}

void Param_Init(FlashParam_t *param){
	param->valid_flag = FLAG_VALID;

	param->boot_version = 0;
	param->boot_run_count = 0;

	param->run_app_version = 0;
	param->run_app_size = 0;
	param->run_app_crc32 = 0;
	param->run_app_error_count = 0;
	param->run_app_status = APP_STATUS_VALID;
	
	param->app_a_version = 0;
	param->app_a_size = 0;
	param->app_a_crc32 = 0;
	param->app_a_status = APP_STATUS_INVALID;
	
	param->app_b_version = 0;
	param->app_b_size = 0;
	param->app_b_crc32 = 0;
	param->app_b_status = APP_STATUS_INVALID;
	
	param->boot_select = APP_AREA_NONE;
	param->update_target = APP_AREA_NONE;
	
	param->crc32 = CalcCRC32((uint32_t *)param, (sizeof(FlashParam_t) - 4U)/4U);
}

void Param_Print(FlashParam_t *param){
    printf("\r\n========== Flash Parameters ==========\r\n");
    printf("Valid Flag:       0x%08dX\r\n", param->valid_flag);
    printf("Boot Version:     V%du.%du.%du\r\n", 
           (param->boot_version >> 16) & 0xFF,
           (param->boot_version >> 8) & 0xFF,
           param->boot_version & 0xFF);
    printf("Boot Run Count:   %du\r\n", param->boot_run_count);
    printf("\r\n");
    
    printf("Run App Version:  V%du.%du.%du\r\n",
           (param->run_app_version >> 16) & 0xFF,
           (param->run_app_version >> 8) & 0xFF,
           param->run_app_version & 0xFF);
    printf("Run App Size:     %du bytes\r\n", param->run_app_size);
    printf("Run App CRC32:    0x%08dX\r\n", param->run_app_crc32);
    printf("Run Error Count:  %du\r\n", param->run_app_error_count);
    printf("Run App Status:   %d\r\n", param->run_app_status);
    printf("\r\n");
    
    printf("App A Version:    V%du.%du.%du\r\n",
           (param->app_a_version >> 16) & 0xFF,
           (param->app_a_version >> 8) & 0xFF,
           param->app_a_version & 0xFF);
    printf("App A Size:       %du bytes\r\n", param->app_a_size);
    printf("App A Status:     %d\r\n", param->app_a_status);
    printf("\r\n");
    
    printf("App B Version:    V%du.%du.%du\r\n",
           (param->app_b_version >> 16) & 0xFF,
           (param->app_b_version >> 8) & 0xFF,
           param->app_b_version & 0xFF);
    printf("App B Size:       %du bytes\r\n", param->app_b_size);
    printf("App B Status:     %d\r\n", param->app_b_status);
    printf("\r\n");
    
    printf("Update Target:    %d\r\n", param->update_target);
    printf("Boot Select:      %d\r\n", param->boot_select);
    printf("CRC32:            0x%08dX\r\n", param->crc32);
    printf("======================================\r\n\r\n");
}

// 打印分区信息
void Flash_PrintPartitionInfo(void){
    printf("\r\n========== Flash Partition Info ==========\r\n");
    printf("Boot区:    0x%08X - 0x%08X (%3d KB)\r\n", 
           BOOT_SECTOR_START_ADDR, BOOT_SECTOR_END_ADDR, BOOT_SECTOR_SIZE/1024);
    printf("参数区:    0x%08X - 0x%08X (%3d KB)\r\n", 
           PARAM_SECTOR_START_ADDR, PARAM_SECTOR_END_ADDR, PARAM_SECTOR_SIZE/1024);
    printf("运行区:    0x%08X - 0x%08X (%3d KB)\r\n", 
           RUN_SECTOR_START_ADDR, RUN_SECTOR_END_ADDR, RUN_SECTOR_SIZE/1024);
    printf("A存储区:   0x%08X - 0x%08X (%3d KB)\r\n", 
           APP_A_SECTOR_START_ADDR, APP_A_SECTOR_END_ADDR, APP_A_SECTOR_SIZE/1024);
    printf("B存储区:   0x%08X - 0x%08X (%3d KB)\r\n", 
           APP_B_SECTOR_START_ADDR, APP_B_SECTOR_END_ADDR, APP_B_SECTOR_SIZE/1024);
    printf("预留区:    0x%08X - 0x%08X (%3d KB)\r\n", 
           RESERVED_SECTOR_START_ADDR, RESERVED_SECTOR_END_ADDR, RESERVED_SECTOR_SIZE/1024);
    printf("==========================================\r\n\r\n");
}

uint8_t Flash_Copy(uint32_t dest_addr, uint32_t src_addr, uint32_t len){
    uint8_t ret;
    uint32_t start_sector;
    uint32_t end_sector;

    if (len == 0U) {
        return 1;
    }

    if ((dest_addr % 4U) != 0U || (src_addr % 4U) != 0U || (len % 4U) != 0U) {
        return 1;
    }

    if (dest_addr < RUN_SECTOR_START_ADDR || (dest_addr + len - 1U) > RUN_SECTOR_END_ADDR) {
        return 1;
    }

    start_sector = Flash_GetSector(dest_addr);
    end_sector = Flash_GetSector(dest_addr + len - 1U);

    ret = Flash_EraseSector(start_sector, end_sector);
    if (ret != 0U){
        return ret;
    }

    ret = Flash_Write(dest_addr, (uint8_t *)src_addr, len);
    if (ret != 0U){
        return ret;
    }

    return 0;
}

// Flash自测试函数
/*uint8_t Flash_SelfTest(void)
{
    uint8_t ret;
    uint8_t write_data[256];
    uint8_t read_data[256];
    uint32_t test_addr = RESERVED_SECTOR_START_ADDR;  // 使用预留区测试
    
    printf("\r\n========== Flash Self Test ==========\r\n");
    
    // 1. 准备测试数据
    for (int i = 0; i < 256; i++) {
        write_data[i] = i;
    }
    printf("[Test] Prepare test data: 256 bytes\r\n");
    
    // 2. 擦除测试区域
    printf("[Test] Erasing sector...\r\n");
    ret = Flash_EraseSector(RESERVED_SECTOR_START, RESERVED_SECTOR_END);
    if (ret != 0) {
        printf("[Test]  Erase FAILED!\r\n");
        return 1;
    }
    printf("[Test]  Erase OK\r\n");
    
    // 3. 写入测试数据
    printf("[Test] Writing data to 0x%08dX...\r\n", test_addr);
    ret = Flash_Write(test_addr, write_data, 256);
    if (ret != 0) {
        printf("[Test]  Write FAILED!\r\n");
        return 1;
    }
    printf("[Test]  Write OK\r\n");
    
    // 4. 读取数据
    printf("[Test] Reading data from 0x%08dX...\r\n", test_addr);
    Flash_Read(test_addr, read_data, 256);
    printf("[Test]  Read OK\r\n");
    
    // 5. 校验数据
    printf("[Test] Verifying data...\r\n");
    for (int i = 0; i < 256; i++) {
        if (write_data[i] != read_data[i]) {
            printf("[Test]  Verify FAILED at offset %d: write=0x%02X, read=0x%02X\r\n",
                   i, write_data[i], read_data[i]);
            return 1;
        }
    }
    printf("[Test]  Verify OK\r\n");
    
    // 6. 打印前16字节数据
    printf("[Test] First 16 bytes:\r\n");
    printf("       ");
    for (int i = 0; i < 16; i++) {
        printf("%02X ", read_data[i]);
    }
    printf("\r\n");
    printf("\r\n[Test]  All tests PASSED!\r\n");
    printf("=====================================\r\n\r\n");
    
    return 0;
}*/

