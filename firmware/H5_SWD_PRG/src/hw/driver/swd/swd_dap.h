#ifndef SWD_DAP_H_
#define SWD_DAP_H_
#include "swd.h"

/* Internal session API. Caller starts the port and releases it on completion.
 * The context identifies the selected AP; all callers must be serialized. */
swd_err_t swdH723Connect(swd_probe_t *p);
swd_err_t swdDapClearErrors(swd_probe_t *p);
swd_err_t swdMemRead32(swd_probe_t *p, uint32_t addr, uint32_t *value);
swd_err_t swdMemWrite32(swd_probe_t *p, uint32_t addr, uint32_t value);
/* Word bursts split at the minimum MEM-AP TAR wrap boundary (1KiB). */
swd_err_t swdMemReadWords(swd_probe_t *p, uint32_t addr, uint32_t *data, uint32_t words);
swd_err_t swdMemWriteWords(swd_probe_t *p, uint32_t addr, const uint32_t *data, uint32_t words);
#endif
