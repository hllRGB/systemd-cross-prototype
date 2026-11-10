// vim: set sw=8 ts=8 et:
#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Some wrappers and frequently used functions.

void * xmalloc(size_t n) {
        void * p = malloc(n ? n : 1);
        if (!p) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }
        return p;
}

void * xrealloc(void * p,
                size_t n) {
        void * q = realloc(p, n ? n : 1);
        if (!q) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }
        return q;
}

char * xstrdup(const char * s) {
        if (!s)
                return NULL;
        char * p = strdup(s);
        if (!p) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }
        return p;
}

char * xasprintf(const char * fmt,
                 ...) {
        va_list ap;
        char * p = NULL;

        va_start(ap, fmt);
        if (vasprintf(&p, fmt, ap) < 0) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }
        va_end(ap);
        return p;
}

void sv_push(strvec * sv,
             const char * s) {
        if (sv->n == sv->cap) {
                sv->cap = sv->cap ? sv->cap * 2 : 8;
                sv->v   = xrealloc(sv->v, sv->cap * sizeof(char *));
        }
        sv->v[sv->n++] = xstrdup(s);
}

int sv_contains(const strvec * sv,
                const char * s) {
        for (size_t i = 0; i < sv->n; i++)
                if (strcmp(sv->v[i], s) == 0)
                        return 1;
        return 0;
}

void sv_free(strvec * sv) {
        for (size_t i = 0; i < sv->n; i++)
                free(sv->v[i]);
        free(sv->v);
        sv->v = NULL;
        sv->n = sv->cap = 0;
}

static void sb_reserve(sbuf * b,
                       size_t extra) {
        if (b->n + extra + 1 <= b->cap)
                return;
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->n + extra + 1)
                cap *= 2;
        b->s   = xrealloc(b->s, cap);
        b->cap = cap;
}

void sb_addn(sbuf * b,
             const char * s,
             size_t n) {
        sb_reserve(b, n);
        memcpy(b->s + b->n, s, n);
        b->n += n;
        b->s[b->n] = 0;
}

void sb_add(sbuf * b,
            const char * s) {
        sb_addn(b, s, strlen(s));
}

void sb_addf(sbuf * b,
             const char * fmt,
             ...) {
        va_list ap;
        char * p = NULL;

        va_start(ap, fmt);
        if (vasprintf(&p, fmt, ap) < 0) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }
        va_end(ap);
        sb_add(b, p);
        free(p);
}

void sb_free(sbuf * b) {
        free(b->s);
        b->s = NULL;
        b->n = b->cap = 0;
}

char * sb_steal(sbuf * b) {
        if (!b->s)
                return xstrdup("");
        char * p = b->s;
        b->s     = NULL;
        b->n = b->cap = 0;
        return p;
}

char * read_whole_file(const char * path,
                       size_t * lenp) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
                return NULL;

        sbuf b = {0};
        char buf[8192];
        for (;;) {
                ssize_t k = read(fd, buf, sizeof buf);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        close(fd);
                        sb_free(&b);
                        return NULL;
                }
                if (k == 0)
                        break;
                sb_addn(&b, buf, (size_t)k);
        }
        close(fd);

        if (lenp)
                *lenp = b.n;
        return sb_steal(&b);
}

int mkdir_p(const char * path) {
        char * tmp = xstrdup(path);
        size_t len = strlen(tmp);

        while (len > 1 && tmp[len - 1] == '/')
                tmp[--len] = 0;

        for (char * p = tmp + 1; *p; p++) {
                if (*p != '/')
                        continue;
                *p = 0;
                if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                        free(tmp);
                        return -1;
                }
                *p = '/';
        }
        int r = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                r = -1;
        free(tmp);
        return r;
}

static const char * const unit_suffixes[] = {
    ".service", ".socket", ".target", ".timer", ".path", ".mount", ".automount", ".swap", ".device", ".slice", ".scope", NULL,
};

int is_unit_name(const char * name) {
        size_t len = strlen(name);
        if (len == 0 || len > 255)
                return 0;
        for (const char * const * s = unit_suffixes; *s; s++)
                if (has_suffix(name, *s) && len > strlen(*s))
                        return 1;
        return 0;
}

char * path_concat(const char * a,
                   const char * b) {
        size_t la = strlen(a), lb = strlen(b);
        int need_slash = la > 0 && a[la - 1] != '/' && !(lb > 0 && b[0] == '/');
        int skip_slash = la > 0 && a[la - 1] == '/' && lb > 0 && b[0] == '/';
        char * r       = xmalloc(la + lb + 2);
        memcpy(r, a, la);
        size_t o = la;
        if (need_slash)
                r[o++] = '/';
        if (skip_slash)
                lb--;
        memcpy(r + o, b + (skip_slash ? 1 : 0), lb);
        o += lb;
        r[o] = 0;
        return r;
}

/* Collapse "//", "." and ".." lexically. Always returns an absolute path
 * if the input is absolute. */
char * path_clean(const char * p) {
        int abs       = p[0] == '/';
        char * copy   = xstrdup(p);
        char ** parts = NULL;
        size_t n = 0, cap = 0;

        for (char * tok = strtok(copy, "/"); tok; tok = strtok(NULL, "/")) {
                if (strcmp(tok, ".") == 0)
                        continue;
                if (strcmp(tok, "..") == 0) {
                        if (n > 0)
                                n--;
                        continue;
                }
                if (n == cap) {
                        cap   = cap ? cap * 2 : 8;
                        parts = xrealloc(parts, cap * sizeof(char *));
                }
                parts[n++] = tok;
        }

        sbuf b = {0};
        if (abs)
                sb_add(&b, "/");
        for (size_t i = 0; i < n; i++) {
                if (i)
                        sb_add(&b, "/");
                sb_add(&b, parts[i]);
        }
        if (b.n == 0)
                sb_add(&b, abs ? "/" : ".");
        free(parts);
        free(copy);
        return sb_steal(&b);
}

char * path_abs(const char * p) {
        if (p[0] == '/')
                return path_clean(p);

        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd))
                return path_clean(p);

        char * j = path_concat(cwd, p);
        char * r = path_clean(j);
        free(j);
        return r;
}

int has_prefix(const char * s,
               const char * p) {
        return strncmp(s, p, strlen(p)) == 0;
}

int has_suffix(const char * s,
               const char * p) {
        size_t ls = strlen(s), lp = strlen(p);
        return ls >= lp && strcmp(s + ls - lp, p) == 0;
}

char * trimdup(const char * s) {
        while (*s == ' ' || *s == '\t')
                s++;
        char * r = xstrdup(s);
        rstrip(r);
        return r;
}

void rstrip(char * s) {
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
                s[--n] = 0;
}
