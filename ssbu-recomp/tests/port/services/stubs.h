/* stubs.h - what nntime.c / nnmii.c / nnapplet.c / nnalbum.c / nnnet.c reach in the rest of the runtime, for the unit tests here. */
#ifndef SVC_STUBS_H
#define SVC_STUBS_H
#include "mt.h"
#include <stdarg.h>
#include <string.h>
#define SVC_MAX_IMPORTS 512
static ImportDef svc_imports[SVC_MAX_IMPORTS];
static int svc_nimports;
void register_imports(const ImportDef *defs, int count) { for (int i = 0; i < count && svc_nimports < SVC_MAX_IMPORTS; i++) svc_imports[svc_nimports++] = defs[i]; }
static ImportFn svc_find(const char *name) { for (int i = 0; i < svc_nimports; i++) if (!strcmp(svc_imports[i].name, name)) return svc_imports[i].fn; return NULL; }
static ImportFn svc_find_sub(const char *sub) { for (int i = 0; i < svc_nimports; i++) if (strstr(svc_imports[i].name, sub)) return svc_imports[i].fn; return NULL; }
#endif
