// vim: set sw=8 ts=8 et:
#define _GNU_SOURCE
#include "index.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// We find unit files.

static const char * const UDIR_SYSTEM[] = {
    "etc/systemd/system",
    "usr/local/lib/systemd/system",
    "usr/lib/systemd/system",
};

static const char * const UDIR_USER[] = {
    "etc/systemd/user",           "usr/local/share/systemd/user", "usr/share/systemd/user",
    "usr/local/lib/systemd/user", "usr/lib/systemd/user",
};

int sidx_nudir(const struct stratum_idx * s) {
        return s->user ? (int)(sizeof UDIR_USER / sizeof UDIR_USER[0]) : (int)(sizeof UDIR_SYSTEM / sizeof UDIR_SYSTEM[0]);
}

const char * sidx_udir(const struct stratum_idx * s,
                       int idx) {
        const char * const * dirs = s->user ? UDIR_USER : UDIR_SYSTEM;
        int n                     = sidx_nudir(s);
        return (idx >= 0 && idx < n) ? dirs[idx] : NULL;
}

static void ent_free(struct uent * e) {
        free(e->name);
        free(e->path);
        free(e->canon);
}

void world_free(struct world * w) {
        for (size_t i = 0; i < w->n; i++) {
                for (size_t j = 0; j < w->v[i].n; j++)
                        ent_free(&w->v[i].v[j]);
                free(w->v[i].v);
                free(w->v[i].name);
                free(w->v[i].root);
        }
        free(w->v);
        memset(w, 0, sizeof *w);
}

struct stratum_idx * world_get(struct world * w,
                               const char * name) {
        for (size_t i = 0; i < w->n; i++)
                if (strcmp(w->v[i].name, name) == 0)
                        return &w->v[i];
        return NULL;
}

static int cmp_ent(const void * a,
                   const void * b) {
        const struct uent *x = a, *y = b;
        int r = strcmp(x->name, y->name);
        if (r)
                return r;
        return x->dir - y->dir;
}

static const char * base_name(const char * p) {
        const char * s = strrchr(p, '/');
        return s ? s + 1 : p;
}

static int under_dir(const char * path,
                     const char * dir) {
        size_t n = strlen(dir);
        return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == 0);
}

static int path_is_in_roots(const struct stratum_idx * s,
                            const char * path) {
        for (int d = 0; d < sidx_nudir(s); d++) {
                char * ud = path_concat(s->root, sidx_udir(s, d));
                int ok    = under_dir(path, ud);
                free(ud);
                if (ok)
                        return 1;
        }
        return 0;
}

static int add_ent(struct stratum_idx * s,
                   struct uent * e) {
        if (s->n % 64 == 0)
                s->v = xrealloc(s->v, (s->n + 64) * sizeof(*s->v));
        s->v[s->n++] = *e;
        return 0;
}

static void scan_dir(struct stratum_idx * s,
                     int d) {
        char * ud = path_concat(s->root, sidx_udir(s, d));
        DIR * dp  = opendir(ud);
        if (!dp) {
                free(ud);
                return;
        }

        struct dirent * de;
        while ((de = readdir(dp))) {
                if (!is_unit_name(de->d_name))
                        continue;

                char * full = path_concat(ud, de->d_name);
                struct stat st;
                if (lstat(full, &st) < 0) {
                        free(full);
                        continue;
                }

                struct uent e = {0};
                e.name        = xstrdup(de->d_name);
                e.dir         = d;

                if (S_ISREG(st.st_mode)) {
                        e.kind = EK_REGULAR;
                        e.path = full;
                        full   = NULL;
                } else if (S_ISLNK(st.st_mode)) {
                        char buf[4096];
                        ssize_t k = readlink(full, buf, sizeof buf - 1);
                        if (k <= 0) {
                                free(full);
                                free(e.name);
                                continue;
                        }
                        buf[k] = 0;

                        free(full);
                        full = NULL;

                        if (strcmp(buf, "/dev/null") == 0) {
                                e.kind = EK_MASKED;
                                e.path = xstrdup("/dev/null");
                        } else {
                                char * raw   = buf[0] == '/' ? path_concat(s->root, buf + 1) : path_concat(ud, buf);
                                char * clean = path_clean(raw);
                                free(raw);

                                if (!under_dir(clean, s->root)) {
                                        free(clean);
                                        free(e.name);
                                        continue;
                                }
                                e.path = clean;
                                if (path_is_in_roots(s, clean)) {
                                        e.kind  = EK_ALIAS;
                                        e.canon = xstrdup(base_name(clean));
                                } else {
                                        e.kind = EK_LINKED;
                                }
                        }
                } else {
                        free(full);
                        free(e.name);
                        continue;
                }

                add_ent(s, &e);
        }
        closedir(dp);
        free(ud);
}

static int build_stratum(struct stratum_idx * s,
                         const char * strata_root,
                         const char * name,
                         int user,
                         char ** err) {
        memset(s, 0, sizeof *s);
        s->name = xstrdup(name);
        s->user = user;
        s->root = path_concat(strata_root, name);

        for (int d = 0; d < sidx_nudir(s); d++)
                scan_dir(s, d);

        qsort(s->v, s->n, sizeof(*s->v), cmp_ent);

        size_t out = 0;
        for (size_t i = 0; i < s->n; i++) {
                if (out > 0 && strcmp(s->v[out - 1].name, s->v[i].name) == 0) {
                        ent_free(&s->v[i]);
                        continue;
                }
                s->v[out++] = s->v[i];
        }
        s->n = out;

        (void)err;
        return 0;
}

int world_init(struct world * w,
               const char * strata_root,
               const strvec * names,
               int user,
               char ** err) {
        memset(w, 0, sizeof *w);
        char * base = path_abs(strata_root);
        for (size_t i = 0; i < names->n; i++) {
                if (world_get(w, names->v[i]))
                        continue;
                w->v = xrealloc(w->v, (w->n + 1) * sizeof(*w->v));
                if (build_stratum(&w->v[w->n], base, names->v[i], user, err) < 0) {
                        free(base);
                        return -1;
                }
                w->n++;
        }
        free(base);
        return 0;
}

struct uent * sidx_find(struct stratum_idx * s,
                        const char * name) {
        size_t lo = 0, hi = s->n;
        while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                int r      = strcmp(s->v[mid].name, name);
                if (r == 0)
                        return &s->v[mid];
                if (r < 0)
                        lo = mid + 1;
                else
                        hi = mid;
        }
        return NULL;
}

static char * template_of(const char * name) {
        const char * at = strchr(name, '@');
        if (!at)
                return NULL;

        const char * dot = strrchr(name, '.');
        if (!dot || dot < at)
                return NULL;
        if (at[1] == '.')
                return NULL; /* already a template */

        size_t plen = (size_t)(at - name);
        char * t    = xasprintf("%.*s@%s", (int)plen, name, dot);
        return t;
}

int sidx_resolve(struct stratum_idx * s,
                 const char * name,
                 struct uent ** out) {
        char * owned     = NULL;
        const char * cur = name;

        for (int depth = 0; depth < 16; depth++) {
                struct uent * e = sidx_find(s, cur);
                if (!e) {
                        if (depth > 0 || !(owned = template_of(name)))
                                break;
                        cur = owned;
                        continue;
                }
                if (e->kind == EK_MASKED) {
                        free(owned);
                        return 0;
                }
                if (e->kind == EK_ALIAS && e->canon) {
                        struct uent * t = sidx_find(s, e->canon);
                        if (t && t->kind != EK_MASKED) {
                                *out = t;
                                free(owned);
                                return 1;
                        }
                }
                *out = e;
                free(owned);
                return 1;
        }

        free(owned);
        return -1;
}
