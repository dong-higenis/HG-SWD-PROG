#ifndef SRC_COMMON_HW_INCLUDE_FATFS_H_
#define SRC_COMMON_HW_INCLUDE_FATFS_H_

#include "hw_def.h"
#include "ff.h"

#ifdef _USE_HW_FATFS

bool fatfsInit(void);
bool fatfsMount(void);
bool fatfsUnMount(void);
bool fatfsIsMount(void);
bool fatfsExist(const char *path);
bool fatfsCreateDir(const char *path);
bool fatfsDelete(const char *path);

bool fatfsOpen(FIL *p_file, const char *path, uint8_t mode);
bool fatfsClose(FIL *p_file);
bool fatfsSync(FIL *p_file);
bool fatfsWrite(FIL *p_file, uint8_t *p_data, uint32_t length);
bool fatfsRead(FIL *p_file, uint8_t *p_data, uint32_t length, uint32_t *p_read_length);



#endif

#endif 
