#ifndef SYSTEMD_CROSS_PLAN_H
#define SYSTEMD_CROSS_PLAN_H

#include "config.h"
#include "index.h"

struct out_unit {
        char *stratum;          /* source stratum */
        char *src;              /* source unit name */
        char *out;              /* generated unit name */
        char *text;             /* rewritten unit text, NULL for local aliases */
        char *alias_path;       /* absolute path of the native unit for local aliases */
        strvec edges;           /* human readable dependency decisions */
        strvec wants;           /* local targets to hook into via .wants */
        strvec requires;        /* local targets to hook into via .requires */
        strvec dep_strata;      /* closure dependencies: providers */
        strvec dep_names;       /* closure dependencies: unit names */
};

struct plan {
        struct config *cfg;
        struct world *w;
        const char *local;

        struct out_unit *v;
        size_t n, cap;

        strvec errors;
        strvec warns;
};

int plan_build(struct plan *p, struct config *cfg, struct world *w);
void plan_free(struct plan *p);

char *plan_prefix_name(const char *stratum, const char *name);

#endif
