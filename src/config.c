#define _GNU_SOURCE
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void set_err(char **err, const char *fmt, ...) {
        if (!err)
                return;
        va_list ap;
        va_start(ap, fmt);
        if (vasprintf(err, fmt, ap) < 0)
                *err = xstrdup("out of memory");
        va_end(ap);
}

void config_free(struct config *c) {
        if (!c)
                return;
        free(c->path);
        free(c->init);
        sv_free(&c->order);
        sv_free(&c->local_only);
        sv_free(&c->ignore);
        for (size_t i = 0; i < c->n_units; i++) {
                free(c->units[i].stratum);
                free(c->units[i].name);
        }
        free(c->units);
        memset(c, 0, sizeof *c);
}

int config_add(struct config *c, const char *stratum, const char *name) {
        for (size_t i = 0; i < c->n_units; i++)
                if (strcmp(c->units[i].stratum, stratum) == 0 &&
                    strcmp(c->units[i].name, name) == 0)
                        return 0;

        if (c->n_units == c->cap_units) {
                c->cap_units = c->cap_units ? c->cap_units * 2 : 8;
                c->units = xrealloc(c->units, c->cap_units * sizeof(*c->units));
        }
        c->units[c->n_units].stratum = xstrdup(stratum);
        c->units[c->n_units].name = xstrdup(name);
        c->n_units++;
        return 0;
}

int config_del(struct config *c, const char *stratum, const char *name) {
        for (size_t i = 0; i < c->n_units; i++) {
                if (strcmp(c->units[i].stratum, stratum) != 0 ||
                    strcmp(c->units[i].name, name) != 0)
                        continue;
                free(c->units[i].stratum);
                free(c->units[i].name);
                memmove(c->units + i, c->units + i + 1,
                        (c->n_units - i - 1) * sizeof(*c->units));
                c->n_units--;
                return 0;
        }
        return -1;
}

static void split_list(const char *value, strvec *out) {
        char *copy = xstrdup(value);
        char *save = NULL;

        for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                char *t = trimdup(tok);
                if (*t)
                        sv_push(out, t);
                free(t);
        }
        free(copy);
}

static int is_template_sec(const char *line, const char *name, const char **arg) {
        size_t n = strlen(name);
        if (strncmp(line, name, n) != 0)
                return 0;
        if (line[n] != ' ' && line[n] != '\t')
                return 0;
        *arg = line + n;
        while (**arg == ' ' || **arg == '\t')
                (*arg)++;
        return 1;
}

int config_load(struct config *c, const char *path, char **err) {
        memset(c, 0, sizeof *c);
        c->path = xstrdup(path);
        c->version = 1;

        FILE *f = fopen(path, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;               /* empty config is valid */
                set_err(err, "%s: %s", path, strerror(errno));
                return -1;
        }

        enum { SEC_NONE, SEC_GLOBAL, SEC_STRATUM, SEC_RESOLVE } sec = SEC_GLOBAL;
        char *stratum = NULL;
        char *line = NULL;
        size_t cap = 0, lineno = 0;

        while (getline(&line, &cap, f) >= 0) {
                lineno++;
                rstrip(line);

                char *s = line;
                while (*s == ' ' || *s == '\t')
                        s++;
                if (*s == 0 || *s == '#' || *s == ';')
                        continue;

                if (*s == '[') {
                        char *end = strchr(s, ']');
                        if (!end) {
                                set_err(err, "%s:%zu: malformed section header", path, lineno);
                                goto fail;
                        }
                        *end = 0;
                        char *body = s + 1;
                        const char *arg = NULL;

                        free(stratum);
                        stratum = NULL;

                        if (is_template_sec(body, "stratum", &arg)) {
                                if (!*arg) {
                                        set_err(err, "%s:%zu: empty stratum name", path, lineno);
                                        goto fail;
                                }
                                stratum = xstrdup(arg);
                                sec = SEC_STRATUM;
                        } else if (strcmp(body, "resolve") == 0) {
                                sec = SEC_RESOLVE;
                        } else if (strcmp(body, "global") == 0) {
                                sec = SEC_GLOBAL;
                        } else {
                                set_err(err, "%s:%zu: unknown section [%s]", path, lineno, body);
                                goto fail;
                        }
                        continue;
                }

                char *eq = strchr(s, '=');
                if (!eq) {
                        set_err(err, "%s:%zu: expected key = value", path, lineno);
                        goto fail;
                }
                *eq = 0;
                char *key = trimdup(s);
                char *val = trimdup(eq + 1);

                int ok = 0;

                if (sec == SEC_GLOBAL && strcmp(key, "version") == 0) {
                        c->version = atoi(val);
                        ok = 1;
                } else if (sec == SEC_RESOLVE && strcmp(key, "init") == 0) {
                        free(c->init);
                        c->init = xstrdup(val);
                        ok = 1;
                } else if (sec == SEC_RESOLVE && strcmp(key, "order") == 0) {
                        split_list(val, &c->order);
                        ok = 1;
                } else if (sec == SEC_RESOLVE && strcmp(key, "local_only") == 0) {
                        split_list(val, &c->local_only);
                        ok = 1;
                } else if (sec == SEC_RESOLVE && strcmp(key, "ignore") == 0) {
                        split_list(val, &c->ignore);
                        ok = 1;
                } else if (sec == SEC_STRATUM && strcmp(key, "units") == 0) {
                        if (!*val) {
                                set_err(err, "%s:%zu: empty unit name", path, lineno);
                                free(key); free(val);
                                goto fail;
                        }
                        config_add(c, stratum, val);
                        ok = 1;
                }

                free(key);
                free(val);

                if (!ok) {
                        set_err(err, "%s:%zu: unknown key for this section", path, lineno);
                        goto fail;
                }
        }

        free(line);
        free(stratum);
        fclose(f);

        if (c->version != 1) {
                set_err(err, "%s: unsupported config version %d", path, c->version);
                return -1;
        }
        return 0;

fail:
        free(line);
        free(stratum);
        fclose(f);
        config_free(c);
        return -1;
}

static int cmp_ref(const void *a, const void *b) {
        const struct unit_ref *x = a, *y = b;
        int r = strcmp(x->stratum, y->stratum);
        if (r)
                return r;
        return strcmp(x->name, y->name);
}

int config_save(const struct config *c) {
        sbuf b = {0};

        sb_addf(&b, "version = %d\n\n", c->version);
        sb_add(&b, "[resolve]\n");
        if (c->init)
                sb_addf(&b, "init = %s\n", c->init);
        if (c->order.n) {
                sb_add(&b, "order = ");
                for (size_t i = 0; i < c->order.n; i++)
                        sb_addf(&b, "%s%s", i ? ", " : "", c->order.v[i]);
                sb_add(&b, "\n");
        }
        if (c->local_only.n) {
                sb_add(&b, "local_only = ");
                for (size_t i = 0; i < c->local_only.n; i++)
                        sb_addf(&b, "%s%s", i ? ", " : "", c->local_only.v[i]);
                sb_add(&b, "\n");
        }
        if (c->ignore.n) {
                sb_add(&b, "ignore = ");
                for (size_t i = 0; i < c->ignore.n; i++)
                        sb_addf(&b, "%s%s", i ? ", " : "", c->ignore.v[i]);
                sb_add(&b, "\n");
        }

        struct unit_ref *sorted = xmalloc((c->n_units ? c->n_units : 1) * sizeof(*sorted));
        memcpy(sorted, c->units, c->n_units * sizeof(*sorted));
        qsort(sorted, c->n_units, sizeof(*sorted), cmp_ref);

        const char *cur = NULL;
        for (size_t i = 0; i < c->n_units; i++) {
                if (!cur || strcmp(cur, sorted[i].stratum) != 0) {
                        cur = sorted[i].stratum;
                        sb_addf(&b, "\n[stratum %s]\n", cur);
                }
                sb_addf(&b, "units = %s\n", sorted[i].name);
        }
        free(sorted);

        char *tmp = xasprintf("%s.tmp.%ld", c->path, (long)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
                free(tmp);
                sb_free(&b);
                return -1;
        }

        size_t off = 0;
        int rc = 0;
        while (off < b.n) {
                ssize_t k = write(fd, b.s + off, b.n - off);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        rc = -1;
                        break;
                }
                off += (size_t)k;
        }
        if (rc == 0 && fsync(fd) < 0)
                rc = -1;
        close(fd);

        if (rc == 0 && rename(tmp, c->path) < 0)
                rc = -1;
        if (rc != 0)
                unlink(tmp);

        free(tmp);
        sb_free(&b);
        return rc;
}
