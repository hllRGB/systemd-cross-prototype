#define _GNU_SOURCE
#include "plan.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void p_add(strvec *sv, const char *fmt, ...) {
        va_list ap;
        char *s = NULL;

        va_start(ap, fmt);
        if (vasprintf(&s, fmt, ap) < 0) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }
        va_end(ap);
        sv_push(sv, s);
        free(s);
}

char *plan_prefix_name(const char *stratum, const char *name) {
        return xasprintf("bedrock-%s-%s", stratum, name);
}

void plan_free(struct plan *p) {
        for (size_t i = 0; i < p->n; i++) {
                free(p->v[i].stratum);
                free(p->v[i].src);
                free(p->v[i].out);
                free(p->v[i].text);
                sv_free(&p->v[i].edges);
                sv_free(&p->v[i].wants);
                sv_free(&p->v[i].requires);
                sv_free(&p->v[i].dep_strata);
                sv_free(&p->v[i].dep_names);
        }
        free(p->v);
        sv_free(&p->errors);
        sv_free(&p->warns);
        memset(p, 0, sizeof *p);
}

/* ------------------------------------------------------------------ */
/* unit text assembly (main file + drop-ins)                           */

static void append_file(sbuf *b, const char *path) {
        size_t len = 0;
        char *t = read_whole_file(path, &len);
        if (!t)
                return;
        sb_addn(b, t, len);
        if (len == 0 || t[len - 1] != '\n')
                sb_add(b, "\n");
        free(t);
}

static void add_variants(strvec *v, const char *name) {
        sv_push(v, name);

        const char *at = strchr(name, '@');
        const char *dot = strrchr(name, '.');
        if (!dot)
                return;

        if (at && dot > at && at[1] != '.') {
                char *t = xasprintf("%.*s@%s", (int)(at - name), name, dot);
                sv_push(v, t);
                free(t);
        }

        size_t blen = (size_t)(dot - name);
        char *base = xmalloc(blen + 1);
        memcpy(base, name, blen);
        base[blen] = 0;

        for (size_t i = blen; i > 0; i--) {
                if (base[i - 1] != '-')
                        continue;
                char *t = xasprintf("%.*s%s", (int)i, base, dot);
                sv_push(v, t);
                free(t);
        }
        free(base);
}

struct dfile {
        char *bn;
        int dir;
        char *path;
};

static int cmp_dfile(const void *a, const void *b) {
        const struct dfile *x = a, *y = b;
        int r = strcmp(x->bn, y->bn);
        if (r)
                return r;
        return x->dir - y->dir;
}

static void collect_dropins(struct stratum_idx *s, const char *variant, sbuf *out) {
        struct dfile *v = NULL;
        size_t n = 0, cap = 0;

        for (int d = 0; d < 3; d++) {
                char *dd = xasprintf("%s/%s/%s.d", s->root, sidx_udir(d), variant);
                DIR *dp = opendir(dd);
                if (!dp) {
                        free(dd);
                        continue;
                }
                struct dirent *de;
                while ((de = readdir(dp))) {
                        if (!has_suffix(de->d_name, ".conf"))
                                continue;
                        if (n == cap) {
                                cap = cap ? cap * 2 : 8;
                                v = xrealloc(v, cap * sizeof(*v));
                        }
                        v[n].bn = xstrdup(de->d_name);
                        v[n].dir = d;
                        v[n].path = path_concat(dd, de->d_name);
                        n++;
                }
                closedir(dp);
                free(dd);
        }

        qsort(v, n, sizeof(*v), cmp_dfile);

        for (size_t i = 0; i < n; i++) {
                if (i > 0 && strcmp(v[i].bn, v[i - 1].bn) == 0) {
                        free(v[i].bn);
                        free(v[i].path);
                        continue;
                }
                append_file(out, v[i].path);
                free(v[i].bn);
                free(v[i].path);
        }
        free(v);
}

static char *load_unit_text(struct stratum_idx *s, struct uent *e, const char *name) {
        sbuf b = {0};
        size_t len = 0;
        char *main_text = read_whole_file(e->path, &len);
        if (!main_text)
                return NULL;

        sb_addn(&b, main_text, len);
        if (len == 0 || main_text[len - 1] != '\n')
                sb_add(&b, "\n");
        free(main_text);

        strvec variants = {0};
        add_variants(&variants, name);
        for (size_t i = 0; i < variants.n; i++)
                collect_dropins(s, variants.v[i], &b);
        sv_free(&variants);

        return sb_steal(&b);
}

/* ------------------------------------------------------------------ */
/* dependency policy                                                   */

static int is_local_only(struct plan *p, const char *token) {
        for (size_t i = 0; i < p->cfg->local_only.n; i++)
                if (fnmatch(p->cfg->local_only.v[i], token, 0) == 0)
                        return 1;
        return 0;
}

/* 0 = local, 1 = foreign (set *prov), -1 = missing */
static int dep_provider(struct plan *p, const char *consumer, const char *token, const char **prov) {
        struct uent *e;

        if (is_local_only(p, token))
                return 0;

        if (strcmp(consumer, p->local) == 0)
                return 0;

        struct stratum_idx *cs = world_get(p->w, consumer);
        if (cs) {
                int r = sidx_resolve(cs, token, &e);
                if (r == 0)
                        return -1;              /* explicitly masked */
                if (r > 0) {
                        *prov = consumer;
                        return 1;
                }
        }

        struct stratum_idx *ls = world_get(p->w, p->local);
        if (ls) {
                int r = sidx_resolve(ls, token, &e);
                if (r > 0)
                        return 0;
        }

        for (size_t i = 0; i < p->cfg->order.n; i++) {
                struct stratum_idx *os = world_get(p->w, p->cfg->order.v[i]);
                if (!os)
                        continue;
                int r = sidx_resolve(os, token, &e);
                if (r > 0) {
                        *prov = p->cfg->order.v[i];
                        return 1;
                }
        }
        return -1;
}

static int is_shared_path(const char *path) {
        static const char *const list[] = {
                "/run", "/tmp", "/var/tmp", "/dev", "/proc", "/sys",
                "/home", "/bedrock", "/shared", "/nix", "/boot", "/efi",
                "/mnt", "/media", NULL,
        };
        for (const char *const *s = list; *s; s++) {
                size_t n = strlen(*s);
                if (strncmp(path, *s, n) == 0 && (path[n] == '/' || path[n] == 0))
                        return 1;
        }
        return 0;
}

static char *qualify_path(struct stratum_idx *src, const char *path) {
        if (path[0] != '/' || is_shared_path(path))
                return xstrdup(path);
        return path_concat(src->root, path + 1);
}

/* ------------------------------------------------------------------ */
/* Exec rewriting                                                      */

static int binary_exists(struct stratum_idx *src, const char *path) {
        char *p = path_concat(src->root, path + 1);
        int r = access(p, X_OK) == 0;
        free(p);
        return r;
}

static char *wrap_exec(struct plan *p, struct stratum_idx *src, const char *name,
                       const char *key, const char *value) {
        char prefixes[8];
        size_t np = 0, i = 0;

        while (value[i]) {
                char c = value[i];
                if (c == '!' && value[i + 1] == '!') {
                        if (np < sizeof prefixes - 1)
                                prefixes[np++] = '!';
                        i += 2;
                        continue;
                }
                if (c == '-' || c == '@' || c == ':' || c == '+' || c == '!') {
                        if (np < sizeof prefixes - 1)
                                prefixes[np++] = c;
                        i++;
                        continue;
                }
                break;
        }
        prefixes[np] = 0;

        const char *rest = value + i;
        if (*rest == 0)
                return xstrdup(value);

        if (*rest != '/') {
                p_add(&p->warns, "%s %s: ExecStart is not absolute, left alone", name, key);
                return xstrdup(value);
        }

        if (strchr(prefixes, '@')) {
                p_add(&p->warns, "%s %s: argv0 override (@) not supported, left alone", name, key);
                return xstrdup(value);
        }

        char *first = xstrdup(rest);
        char *sp = strpbrk(first, " \t");
        if (sp)
                *sp = 0;
        if (!binary_exists(src, first))
                p_add(&p->warns, "%s %s: %s not executable in stratum, still wrapping",
                      name, key, first);
        free(first);

        sbuf b = {0};
        sb_add(&b, prefixes);
        sb_addf(&b, "/bedrock/bin/strat -r %s %s", src->name, rest);
        return sb_steal(&b);
}

/* ------------------------------------------------------------------ */
/* line oriented transform                                             */

enum section { SEC_OTHER, SEC_UNIT, SEC_SERVICE, SEC_INSTALL };

static int is_closure_key(const char *k) {
        return strcmp(k, "Requires") == 0 || strcmp(k, "Wants") == 0 ||
               strcmp(k, "Requisite") == 0 || strcmp(k, "BindsTo") == 0 ||
               strcmp(k, "Upholds") == 0;
}

static int is_order_key(const char *k) {
        static const char *const keys[] = {
                "After", "Before", "Conflicts", "PartOf",
                "PropagatesReloadTo", "ReloadPropagatedFrom", "JoinsNamespaceOf",
                "OnFailure", "OnSuccess", NULL,
        };
        for (const char *const *s = keys; *s; s++)
                if (strcmp(k, *s) == 0)
                        return 1;
        return 0;
}

static int is_exec_key(const char *k) {
        static const char *const keys[] = {
                "ExecStart", "ExecStartPre", "ExecStartPost",
                "ExecStop", "ExecStopPost", "ExecReload",
                "ExecCondition", NULL,
        };
        for (const char *const *s = keys; *s; s++)
                if (strcmp(k, *s) == 0)
                        return 1;
        return 0;
}

static int is_fatal_key(const char *k, const char *v) {
        if (strcmp(k, "RootDirectory") == 0 || strcmp(k, "RootImage") == 0 ||
            strcmp(k, "RootImageOptions") == 0)
                return 1;
        if (strcmp(k, "DynamicUser") == 0 && (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0))
                return 1;
        if (strcmp(k, "PrivateUsers") == 0 && (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0))
                return 1;
        return 0;
}

static void rewrite_dep_list(struct plan *p, struct stratum_idx *src, const char *name,
                             const char *key, const char *value, int closure,
                             struct out_unit *ou, sbuf *out) {
        char *copy = xstrdup(value);
        char *save = NULL;
        int first = 1;

        for (char *tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
                if (!first)
                        sb_add(out, " ");
                first = 0;

                if (strchr(tok, '%')) {
                        p_add(&p->warns, "%s: specifier in %s=%s left alone", name, key, tok);
                        sb_add(out, tok);
                        continue;
                }

                const char *prov = NULL;
                int r = dep_provider(p, src->name, tok, &prov);

                if (r < 0) {
                        if (closure)
                                p_add(&p->errors, "%s: dependency %s cannot be resolved", name, tok);
                        else
                                p_add(&p->warns, "%s: ordering dependency %s unresolved", name, tok);
                        sb_add(out, tok);
                        p_add(&ou->edges, "%s=%s -> missing", key, tok);
                        continue;
                }

                if (r == 0) {
                        sb_add(out, tok);
                        p_add(&ou->edges, "%s=%s -> local", key, tok);
                        continue;
                }

                char *pref = plan_prefix_name(prov, tok);
                sb_add(out, pref);
                p_add(&ou->edges, "%s=%s -> %s", key, tok, prov);
                free(pref);

                if (closure) {
                        sv_push(&ou->dep_strata, prov);
                        sv_push(&ou->dep_names, tok);
                }
        }
        free(copy);
}

static void rewrite_install_list(struct plan *p, struct stratum_idx *src, const char *name,
                                 const char *key, const char *value, struct out_unit *ou,
                                 sbuf *out, int require) {
        char *copy = xstrdup(value);
        char *save = NULL;
        int first = 1;

        for (char *tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
                const char *prov = NULL;
                int r = dep_provider(p, src->name, tok, &prov);

                if (r != 0) {
                        p_add(&p->warns, "%s: [Install] %s=%s is not a local target, link skipped",
                              name, key, tok);
                        continue;
                }
                if (!first)
                        sb_add(out, " ");
                first = 0;
                sb_add(out, tok);
                sv_push(require ? &ou->requires : &ou->wants, tok);
        }
        free(copy);
}

static char *next_logical_line(char **cur) {
        if (!*cur || !**cur)
                return NULL;

        sbuf b = {0};
        char *p = *cur;

        for (;;) {
                char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                char *seg = xmalloc(len + 1);
                memcpy(seg, p, len);
                seg[len] = 0;
                p = nl ? nl + 1 : NULL;

                int cont = 0;
                size_t l = strlen(seg);
                if (l > 0) {
                        size_t bs = 0;
                        while (bs < l && seg[l - 1 - bs] == '\\')
                                bs++;
                        if (bs % 2 == 1) {
                                seg[l - 1] = 0;
                                cont = 1;
                        }
                }

                sb_add(&b, seg);
                free(seg);

                if (cont && p) {
                        sb_add(&b, " ");
                        continue;
                }
                break;
        }

        *cur = p;
        return sb_steal(&b);
}

static char *transform_text(struct plan *p, struct stratum_idx *src, const char *name,
                            const char *text, struct out_unit *ou) {
        sbuf out = {0};
        char *cur = (char *)text;
        enum section sec = SEC_OTHER;

        for (;;) {
                char *raw = next_logical_line(&cur);
                if (!raw)
                        break;

                rstrip(raw);
                char *s = raw;
                while (*s == ' ' || *s == '\t')
                        s++;

                if (*s == 0) {
                        free(raw);
                        continue;
                }
                if (*s == '#' || *s == ';') {
                        sb_add(&out, s);
                        sb_add(&out, "\n");
                        free(raw);
                        continue;
                }

                if (*s == '[') {
                        if (strcmp(s, "[Unit]") == 0)
                                sec = SEC_UNIT;
                        else if (strcmp(s, "[Service]") == 0)
                                sec = SEC_SERVICE;
                        else if (strcmp(s, "[Install]") == 0)
                                sec = SEC_INSTALL;
                        else
                                sec = SEC_OTHER;
                        sb_add(&out, s);
                        sb_add(&out, "\n");
                        free(raw);
                        continue;
                }

                char *eq = strchr(s, '=');
                if (!eq) {
                        sb_add(&out, s);
                        sb_add(&out, "\n");
                        free(raw);
                        continue;
                }
                *eq = 0;
                char *key = trimdup(s);
                char *val = trimdup(eq + 1);

                sbuf newval = {0};
                sb_add(&newval, val);

                if (sec == SEC_UNIT) {
                        if (strcmp(key, "Description") == 0 && val[0] != '[') {
                                sb_free(&newval);
                                sb_addf(&newval, "[%s] %s", src->name, val);
                        } else if (is_closure_key(key)) {
                                sb_free(&newval);
                                rewrite_dep_list(p, src, name, key, val, 1, ou, &newval);
                        } else if (is_order_key(key)) {
                                sb_free(&newval);
                                rewrite_dep_list(p, src, name, key, val, 0, ou, &newval);
                        }
                } else if (sec == SEC_SERVICE) {
                        if (is_exec_key(key)) {
                                sb_free(&newval);
                                char *w = wrap_exec(p, src, name, key, val);
                                sb_add(&newval, w);
                                free(w);
                        } else if (strcmp(key, "EnvironmentFile") == 0) {
                                const char *v = val;
                                sb_free(&newval);
                                if (*v == '-') {
                                        sb_add(&newval, "-");
                                        v++;
                                }
                                char *q = qualify_path(src, v);
                                sb_add(&newval, q);
                                free(q);
                        } else if (strcmp(key, "WorkingDirectory") == 0) {
                                if (val[0] == '/' && strcmp(val, "/") != 0 && !is_shared_path(val))
                                        p_add(&p->warns,
                                              "%s: WorkingDirectory=%s is outside shared paths", name, val);
                        } else if (strcmp(key, "StandardOutput") == 0 || strcmp(key, "StandardError") == 0) {
                                if (has_prefix(val, "file:") || has_prefix(val, "append:"))
                                        p_add(&p->warns, "%s: %s=%s not rewritten", name, key, val);
                        } else if (is_fatal_key(key, val)) {
                                p_add(&p->errors, "%s: unsupported directive %s=", name, key);
                        }
                } else if (sec == SEC_INSTALL) {
                        if (strcmp(key, "WantedBy") == 0) {
                                sb_free(&newval);
                                rewrite_install_list(p, src, name, key, val, ou, &newval, 0);
                        } else if (strcmp(key, "RequiredBy") == 0) {
                                sb_free(&newval);
                                rewrite_install_list(p, src, name, key, val, ou, &newval, 1);
                        } else if (strcmp(key, "Alias") == 0 || strcmp(key, "Also") == 0) {
                                p_add(&p->warns, "%s: [Install] %s not ported", name, key);
                                sb_free(&newval);
                                sb_add(&newval, val);
                        }
                }

                sb_addf(&out, "%s=%s\n", key, newval.s ? newval.s : "");
                sb_free(&newval);
                free(key);
                free(val);
                free(raw);
        }

        return sb_steal(&out);
}

/* ------------------------------------------------------------------ */
/* plan construction                                                   */

static char *visit_key(const char *stratum, const char *name) {
        return xasprintf("%s\x1f%s", stratum, name);
}

int plan_build(struct plan *p, struct config *cfg, struct world *w) {
        memset(p, 0, sizeof *p);
        p->cfg = cfg;
        p->w = w;

        if (!cfg->init || !*cfg->init) {
                p_add(&p->errors, "config: [resolve] init is not set");
                return -1;
        }
        p->local = cfg->init;

        if (!world_get(w, p->local)) {
                p_add(&p->errors, "config: init stratum '%s' not present", p->local);
                return -1;
        }

        struct unit_ref *queue = NULL;
        size_t qn = 0, qcap = 0;
        strvec visited = {0};

        for (size_t i = 0; i < cfg->n_units; i++) {
                if (strcmp(cfg->units[i].stratum, p->local) == 0) {
                        p_add(&p->warns, "%s/%s: local stratum unit needs no porting",
                              cfg->units[i].stratum, cfg->units[i].name);
                        continue;
                }
                if (qn == qcap) {
                        qcap = qcap ? qcap * 2 : 8;
                        queue = xrealloc(queue, qcap * sizeof(*queue));
                }
                queue[qn].stratum = xstrdup(cfg->units[i].stratum);
                queue[qn].name = xstrdup(cfg->units[i].name);
                qn++;
        }

        for (size_t head = 0; head < qn; head++) {
                const char *sname = queue[head].stratum;
                const char *uname = queue[head].name;

                char *key = visit_key(sname, uname);
                if (sv_contains(&visited, key)) {
                        free(key);
                        continue;
                }
                sv_push(&visited, key);
                free(key);

                struct stratum_idx *src = world_get(w, sname);
                if (!src) {
                        p_add(&p->errors, "%s/%s: unknown stratum", sname, uname);
                        continue;
                }

                struct uent *e = NULL;
                int r = sidx_resolve(src, uname, &e);
                if (r < 0) {
                        p_add(&p->errors, "%s/%s: unit not found", sname, uname);
                        continue;
                }
                if (r == 0) {
                        p_add(&p->errors, "%s/%s: unit is masked", sname, uname);
                        continue;
                }

                char *text = load_unit_text(src, e, uname);
                if (!text) {
                        p_add(&p->errors, "%s/%s: cannot read %s", sname, uname, e->path);
                        continue;
                }

                if (p->n == p->cap) {
                        p->cap = p->cap ? p->cap * 2 : 8;
                        p->v = xrealloc(p->v, p->cap * sizeof(*p->v));
                }
                struct out_unit *ou = &p->v[p->n];
                memset(ou, 0, sizeof *ou);
                ou->stratum = xstrdup(sname);
                ou->src = xstrdup(uname);
                ou->out = plan_prefix_name(sname, uname);
                ou->text = transform_text(p, src, uname, text, ou);
                free(text);
                p->n++;

                if (strlen(ou->out) > 255)
                        p_add(&p->errors, "%s/%s: generated name too long", sname, uname);

                struct stratum_idx *ls = world_get(w, p->local);
                if (ls && sidx_find(ls, ou->out))
                        p_add(&p->errors, "%s: generated name collides with local unit", ou->out);

                for (size_t i = 0; i < ou->dep_names.n; i++) {
                        const char *ds = ou->dep_strata.v[i];
                        const char *dn = ou->dep_names.v[i];
                        if (strcmp(ds, p->local) == 0)
                                continue;
                        if (qn == qcap) {
                                qcap = qcap ? qcap * 2 : 8;
                                queue = xrealloc(queue, qcap * sizeof(*queue));
                        }
                        queue[qn].stratum = xstrdup(ds);
                        queue[qn].name = xstrdup(dn);
                        qn++;
                }
        }

        for (size_t i = 0; i < qn; i++) {
                free(queue[i].stratum);
                free(queue[i].name);
        }
        free(queue);
        sv_free(&visited);

        return p->errors.n ? -1 : 0;
}
