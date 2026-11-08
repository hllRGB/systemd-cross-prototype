#ifndef SYSTEMD_CROSS_CONFIG_H
#define SYSTEMD_CROSS_CONFIG_H

#include "util.h"

struct unit_ref {
        char *stratum;
        char *name;
};

struct config {
        char *path;
        int version;

        char *init;             /* init-providing stratum, treated as local */
        strvec order;           /* fallback provider order, excluding local/init */
        strvec local_only;      /* fnmatch patterns resolved to the init stratum */
        strvec ignore;          /* unit names never ported (optional) */

        struct unit_ref *units;
        size_t n_units, cap_units;
};

int config_load(struct config *c, const char *path, char **err);
int config_save(const struct config *c);
void config_free(struct config *c);

int config_add(struct config *c, const char *stratum, const char *name);
int config_del(struct config *c, const char *stratum, const char *name);

#endif
