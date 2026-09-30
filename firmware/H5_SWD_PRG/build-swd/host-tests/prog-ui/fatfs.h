
#pragma once
#include "hw_def.h"
#include <stdio.h>
typedef unsigned UINT;
typedef unsigned FRESULT;
typedef struct { FILE *fp; uint32_t size; } FIL;
typedef struct { unsigned index; } DIR;
typedef struct { char fname[256]; unsigned fattrib; uint32_t fsize; } FILINFO;
#define FR_OK 0
#define FR_INT_ERR 2
#define FA_READ 1
#define AM_DIR 16
#define f_size(p) ((p)->size)
bool fatfsIsMount(void);
FRESULT f_open(FIL *,const char *,unsigned);
FRESULT f_read(FIL *,void *,UINT,UINT *);
FRESULT f_lseek(FIL *,uint32_t);
FRESULT f_close(FIL *);
FRESULT f_stat(const char *,FILINFO *);
FRESULT f_opendir(DIR *,const char *);
FRESULT f_readdir(DIR *,FILINFO *);
FRESULT f_closedir(DIR *);
