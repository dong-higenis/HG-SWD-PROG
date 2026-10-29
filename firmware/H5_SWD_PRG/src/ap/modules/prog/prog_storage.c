#include "prog_storage.h"
#include <string.h>

#define WIPE_DEPTH 8 /* Folder levels below the root; each level keeps one DIR on the stack. */

typedef struct
{
  const prog_storage_input_t *input;
  prog_storage_result_t      *result;
  bool                        deleting;
  uint32_t                    done;
  uint32_t                    total;
  const char                 *error;
  char                        path[256];
  FILINFO                     info; /* Shared by every level; read before descending. */
} wipe_t;

static bool progStorageIo(wipe_t *w, FRESULT e)
{
  if (e != FR_OK && !w->error)
  {
    w->result->io = e;
    w->error      = "SD error";
  }
  return e == FR_OK;
}

/* path[0..length) names a folder ("" is the root). Children go first so each folder is empty when it is deleted. */
static bool progStorageFolder(wipe_t *w, size_t length, unsigned depth)
{
  DIR dir;
  if (!progStorageIo(w, f_opendir(&dir, length ? w->path : "/")))
  {
    return false;
  }
  FRESULT e;
  while ((e = f_readdir(&dir, &w->info)) == FR_OK && w->info.fname[0])
  {
    size_t  n         = strlen(w->info.fname);
    uint8_t attribute = w->info.fattrib;
    if (length + 1 + n >= sizeof(w->path))
    {
      w->error = "Path too long";
      break;
    }
    w->path[length] = '/';
    memcpy(w->path + length + 1, w->info.fname, n + 1);
    if (attribute & AM_DIR)
    {
      if (depth == WIPE_DEPTH)
      {
        w->error = "Folder too deep";
        break;
      }
      if (!progStorageFolder(w, length + 1 + n, depth + 1))
      {
        break;
      }
    }
    if (w->deleting)
    {
      if ((attribute & AM_RDO) && !progStorageIo(w, f_chmod(w->path, 0, AM_RDO)))
      {
        break;
      }
      if (!progStorageIo(w, f_unlink(w->path)))
      {
        break;
      }
      w->result->removed++;
    }
    w->done++;
    const char *stage = w->deleting ? "DELETE" : "SCAN SD";
    if (!w->input->progress(w->input->context, stage, w->deleting ? w->done : 0, w->total))
    {
      w->error = "Stopped";
      break;
    }
  }
  progStorageIo(w, e);
  w->path[length] = 0;
  progStorageIo(w, f_closedir(&dir));
  return !w->error;
}

const char *progStorageWipe(const prog_storage_input_t *input, prog_storage_result_t *result)
{
  static wipe_t w; /* 300 bytes of path and name; kept off the caller's stack. */
  memset(&w, 0, sizeof(w));
  memset(result, 0, sizeof(*result));
  w.input  = input;
  w.result = result;
  if (progStorageFolder(&w, 0, 0))
  {
    w.total    = w.done ? w.done : 1;
    w.done     = 0;
    w.deleting = true;
    progStorageFolder(&w, 0, 0);
  }
  return w.error;
}
