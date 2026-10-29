#ifndef PROG_CATALOG_H
#define PROG_CATALOG_H
#include "fatfs.h"
#include "esp_project.h"
#include "prog_inspect.h"
#include "prog_target.h"

/* One SD package is one Hub project: /prog/fw/<id>/fw.txt, its BIN files and, from Hub packages, release.txt. */
#define PROG_CATALOG_SIZE 16
#define PROG_ID_SIZE      25
#define PROG_VERSION_SIZE 32

typedef struct
{
  prog_target_t   target; /* release.txt chip= checked against device=, else device=; set even when fw.txt is rejected. */
  bool            release; /* A usable release.txt was found. */
  char            chip[16];
  char            version[PROG_VERSION_SIZE]; /* Empty when unknown. */
  bool            esp_project;
  prog_manifest_t manifest; /* name is valid for ESP packages too. */
  esp_project_t   esp;
} prog_package_t;

typedef struct
{
  char          id[PROG_ID_SIZE];
  char          name[48];
  char          version[PROG_VERSION_SIZE];
  prog_target_t target;
  uint8_t       images;
  uint32_t      size;
  const char   *error; /* NULL when the package can be programmed. */
} prog_catalog_entry_t;

typedef struct
{
  prog_catalog_entry_t entries[PROG_CATALOG_SIZE]; /* Sorted by target, name, then ID. */
  unsigned             count;
  bool                 more; /* More packages on the card; the smallest IDs (strcmp order) were kept. */
} prog_catalog_t;

bool        progCatalogIsValidId(const char *id);
/* Reads release.txt and checks fw.txt; BIN contents are checked by the programming preflight. */
const char *progPackageLoad(const char *id, prog_package_t *out, FRESULT *io);
FRESULT     progCatalogScan(prog_catalog_t *catalog);
/* Indices of one target's packages, in catalog order. PROG_TARGET_NONE selects unknown devices. */
unsigned    progCatalogFilter(const prog_catalog_t *catalog, prog_target_t target, bool usable_only, uint8_t *index,
                              unsigned size);
#endif
