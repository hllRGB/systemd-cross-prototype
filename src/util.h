// vim: set sw=8 ts=8 et:
#ifndef SYSTEMD_CROSS_UTIL_H
#define SYSTEMD_CROSS_UTIL_H

#include <stddef.h>

void * xmalloc(size_t n);
void * xrealloc(void * p,
                size_t n);
char * xstrdup(const char * s);
char * xasprintf(const char * fmt,
                 ...) __attribute__((format(printf,
                                            1,
                                            2)));

/* growable string vector */
typedef struct {
        char ** v;
        size_t n, cap;
} strvec;

void sv_push(strvec * sv,
             const char * s);
int sv_contains(const strvec * sv,
                const char * s);
void sv_free(strvec * sv);

/* growable string buffer */
typedef struct {
        char * s;
        size_t n, cap;
} sbuf;

void sb_add(sbuf * b,
            const char * s);
void sb_addn(sbuf * b,
             const char * s,
             size_t n);
void sb_addf(sbuf * b,
             const char * fmt,
             ...) __attribute__((format(printf,
                                        2,
                                        3)));
void sb_free(sbuf * b);
char * sb_steal(sbuf * b);

char * read_whole_file(const char * path,
                       size_t * lenp);
int mkdir_p(const char * path);
int is_unit_name(const char * name);

char * path_concat(const char * a,
                   const char * b);
char * path_clean(const char * p); /* lexical normalisation, no fs access */
char * path_abs(const char * p);   /* cwd-relative to absolute + clean */

int has_prefix(const char * s,
               const char * p);
int has_suffix(const char * s,
               const char * p);

char * trimdup(const char * s);
void rstrip(char * s);

#endif
