#ifndef MODULE_H_
#define MODULE_H_


#ifdef __cplusplus
extern "C" {
#endif

#include "ap_def.h"




typedef enum
{
  MODULE_PRI_HIGH = 1,
  MODULE_PRI_1,
  MODULE_PRI_2,
  MODULE_PRI_3,
  MODULE_PRI_4,
  MODULE_PRI_NORMAL,
  MODULE_PRI_LOWEST,
  MODULE_PRI_MAX,
} ModulePriority_t;



typedef struct module_t_
{
  const char        name[32];
  ModulePriority_t  priority;
  bool            (*init)(void);
} module_t;


#define MODULE_DEF(x_name) static __attribute__((section(".module"))) volatile module_t module_##x_name = 




bool moduleInit(void);

#ifdef __cplusplus
}
#endif

#endif