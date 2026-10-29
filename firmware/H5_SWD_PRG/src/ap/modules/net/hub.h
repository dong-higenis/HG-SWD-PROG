#ifndef HUB_H
#define HUB_H
#include <stdbool.h>
#include <stdint.h>
#include "prog/prog_catalog.h"

/* HG-SWD FW Hub on GitHub Pages: manifest.json lists the projects, fw/<id>/latest.txt the files to fetch. */
#define HUB_URL      "https://can-dubu.github.io/hg-swd-fw-hub/"
#define HUB_PROJECTS 32
#define HUB_FILES    12

typedef struct
{
  char          id[PROG_ID_SIZE];
  char          name[48];
  char          chip[16];
  char          version[PROG_VERSION_SIZE]; /* "latest" in the manifest. */
  prog_target_t target;                     /* PROG_TARGET_NONE: a chip this firmware cannot write. */
} hub_project_t;

typedef struct
{
  hub_project_t projects[HUB_PROJECTS]; /* Manifest order. */
  unsigned      count;
  bool          more;
} hub_list_t;

/* Streaming reader for the "projects" array of manifest.json; fed in chunks of any size. */
typedef struct
{
  uint8_t       depth;
  bool          in_string;
  bool          escape;
  uint8_t       unicode; /* Hex digits of \uXXXX still to skip. */
  bool          object[8]; /* Container at each depth is an object (else an array). */
  bool          expect_key[8];
  char          key[8][12];
  char          text[64];
  uint8_t       text_len;
  bool          projects;  /* Inside root "projects". */
  bool          in_project; /* Inside one project object. */
  hub_project_t current;
  hub_list_t   *list;
  bool          error;
} hub_json_t;

void hubJsonBegin(hub_json_t *json, hub_list_t *list);
void hubJsonFeed(hub_json_t *json, const uint8_t *data, uint32_t size);

typedef struct
{
  char     name[48];
  uint32_t size;
  uint32_t crc32;
} hub_file_t;

typedef struct
{
  char       id[PROG_ID_SIZE];
  char       version[PROG_VERSION_SIZE];
  char       base[96];
  hub_file_t files[HUB_FILES]; /* In download order; release.txt and fw.txt come last. */
  unsigned   count;
} hub_latest_t;

/* fw/<id>/latest.txt; text is modified. Returns NULL or the reason it cannot be used. */
const char *hubLatestParse(char *text, hub_latest_t *out);

typedef bool (*hub_progress_t)(void *context, const char *stage, uint32_t done, uint32_t total);

/* Network jobs; the caller owns the SD and the ESP link. Return NULL or a short reason for the OLED. */
const char *hubFetchList(hub_list_t *list, hub_progress_t progress, void *context);
/* Downloads every file of the latest version into /prog/fw/<id>/ (each via a .part file, checked against
 * its size and CRC32); fw.txt is removed first and written last, so a broken install is never selectable. */
const char *hubInstall(const hub_project_t *project, hub_progress_t progress, void *context);
#endif
