#ifndef SYSTEM_H_
#define SYSTEM_H_


#ifdef __cplusplus
extern "C" {
#endif

#include "ap_def.h"

#define lock(x)      xSemaphoreTake(x, portMAX_DELAY);
#define unLock(x)    xSemaphoreGive(x);

bool systemInit(void);
void systemMain(void);
bool systemWaitStart(void);

#ifdef __cplusplus
}
#endif

#endif