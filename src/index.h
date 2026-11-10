// vim: set sw=8 ts=8 et:
#ifndef SYSTEMD_CROSS_INDEX_H
#define SYSTEMD_CROSS_INDEX_H

#include "util.h"

enum ekind {
        EK_REGULAR,
        EK_MASKED,
        EK_ALIAS,
        EK_LINKED,
};

struct uent {
        char * name;
        enum ekind kind;
        int dir;      /* index into the stratum unit dir list */
        char * path;  /* host path to read content from */
        char * canon; /* canonical name for aliases, else NULL */
};

struct stratum_idx {
        char * name;
        char * root;
        int user; /* 0 = system, 1 = user manager */
        struct uent * v;
        size_t n;
};

struct world {
        struct stratum_idx * v;
        size_t n;
};

int world_init(struct world * w,
               const char * strata_root,
               const strvec * names,
               int user,
               char ** err);
void world_free(struct world * w);
struct stratum_idx * world_get(struct world * w,
                               const char * name);

struct uent * sidx_find(struct stratum_idx * s,
                        const char * name);

/* 1 = found, 0 = masked, -1 = not found */
int sidx_resolve(struct stratum_idx * s,
                 const char * name,
                 struct uent ** out);

int sidx_nudir(const struct stratum_idx * s);
const char * sidx_udir(const struct stratum_idx * s,
                       int idx);

#endif
