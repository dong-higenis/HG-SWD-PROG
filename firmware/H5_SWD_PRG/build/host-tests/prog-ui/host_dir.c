
#include <dirent.h>
#include <string.h>
void *hostOpenDir(const char *path) { return opendir(path); }
int hostCloseDir(void *dir) { return closedir(dir); }
/* Next name other than "." and "..", or NULL. */
const char *hostReadDir(void *dir) {
  struct dirent *e;
  while((e=readdir(dir)) && (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")));
  return e?e->d_name:NULL;
}
