#ifndef CLI_H_
#define CLI_H_

// USB Serial debug console. Commands: help, status, scan, wifi, lte, off, get, log.
void cliInit(void);
void cliTask(void);

#endif
