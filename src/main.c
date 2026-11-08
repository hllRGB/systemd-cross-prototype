#define _GNU_SOURCE
#include "config.h"
#include "index.h"
#include "plan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct opts {
        const char *config;
        const char *strata_root;
        const char *local;
        int force;
        int no_reload;
        int dry;
};

static int use_color_out, use_color_err;

#define RST "\033[0m"
#define BOLD "\033[1m"
#define RED "\033[91m"
#define YEL "\033[93m"
#define CYA "\033[36m"
#define GRN "\033[32m"

static const char *oc(const char *code) { return use_color_out ? code : ""; }
static const char *ec(const char *code) { return use_color_err ? code : ""; }

static void errf(const char *fmt, ...) {
        va_list ap;
        char *msg = NULL;

        va_start(ap, fmt);
        if (vasprintf(&msg, fmt, ap) < 0)
                msg = NULL;
        va_end(ap);

        fprintf(stderr, "%serror:%s %s\n", ec(RED), ec(RST), msg ? msg : fmt);
        free(msg);
}

static void usage(void) {
        fprintf(stderr,
                "usage: systemd-cross [options] <command> [args]\n"
                "\n"
                "commands:\n"
                "  setup <stratum> <unit>   add an explicit port entry to the config\n"
                "  delete <stratum> <unit>  remove an explicit port entry\n"
                "  import <stratum>         add all units enabled in that stratum\n"
                "  list                     show configured units and their plan status\n"
                "  tree                     show resolved dependency edges\n"
                "  check                    analyse only, no writes\n"
                "  generate <dir>           generator mode: write units into <dir>\n"
                "\n"
                "options:\n"
                "  --config <path>       config file (default /bedrock/etc/global-units.conf)\n"
                "  --strata-root <path>  strata root (default /bedrock/strata)\n"
                "  --local <stratum>     override [resolve] init\n"
                "  --force               save even if analysis failed\n"
                "  --dry-run             print what import would add, do not write\n");
}

static void collect_strata(const struct config *cfg, strvec *out) {
        if (cfg->init && *cfg->init && !sv_contains(out, cfg->init))
                sv_push(out, cfg->init);
        for (size_t i = 0; i < cfg->order.n; i++)
                if (!sv_contains(out, cfg->order.v[i]))
                        sv_push(out, cfg->order.v[i]);
        for (size_t i = 0; i < cfg->n_units; i++)
                if (!sv_contains(out, cfg->units[i].stratum))
                        sv_push(out, cfg->units[i].stratum);
}

static void print_diags(const struct plan *p, int errors) {
        if (errors) {
                for (size_t i = 0; i < p->errors.n; i++)
                        errf("%s", p->errors.v[i]);
        }
        for (size_t i = 0; i < p->warns.n; i++)
                fprintf(stderr, "%swarning:%s %s\n", ec(YEL), ec(RST), p->warns.v[i]);
}

static char *detect_init_stratum(const char *strata_root) {
        const char *env = getenv("BRL_INIT_STRATUM");
        if (env && *env)
                return xstrdup(env);

        char *link = path_concat(strata_root, "init");
        char *res = realpath(link, NULL);
        free(link);
        if (!res)
                return NULL;

        const char *base = strrchr(res, '/');
        char *name = xstrdup(base ? base + 1 : res);
        free(res);
        return name;
}

static int normalize_init(struct opts *o, struct config *cfg) {
        if (o->local) {
                free(cfg->init);
                cfg->init = xstrdup(o->local);
                return 0;
        }
        if (cfg->init && strcmp(cfg->init, "auto") == 0) {
                char *d = detect_init_stratum(o->strata_root);
                if (!d) {
                        errf("cannot detect init stratum");
                        return -1;
                }
                free(cfg->init);
                cfg->init = d;
        }
        return 0;
}

static int analyse(struct opts *o, struct config *cfg, struct plan *p,
                   struct world *w, strvec *strata) {
        memset(p, 0, sizeof *p);

        if (normalize_init(o, cfg) < 0)
                return -1;

        collect_strata(cfg, strata);
        char *err = NULL;
        if (world_init(w, o->strata_root, strata, &err) < 0) {
                errf("%s", err ? err : "world init failed");
                free(err);
                return -1;
        }
        int r = plan_build(p, cfg, w);
        print_diags(p, r < 0);
        return r;
}

static int symlink_relative(const char *unit, const char *linkpath) {
        char *t = xasprintf("../%s", unit);
        int r = symlink(t, linkpath);
        free(t);
        return r;
}

static int write_file(const char *path, const char *text) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
                return -1;

        size_t len = strlen(text), off = 0;
        int rc = 0;
        while (off < len) {
                ssize_t k = write(fd, text + off, len - off);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        rc = -1;
                        break;
                }
                off += (size_t)k;
        }
        close(fd);
        return rc;
}

static int cmd_generate(struct opts *o, char **args, int nargs) {
        if (nargs < 1) {
                usage();
                return 2;
        }
        const char *outdir = args[0];

        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);
        if (r < 0) {
                errf("not generating any unit");
                config_free(&cfg);
                world_free(&w);
                sv_free(&strata);
                plan_free(&p);
                return 1;
        }

        if (mkdir_p(outdir) < 0) {
                errf("cannot create %s", outdir);
                return 1;
        }

        for (size_t i = 0; i < p.n; i++) {
                struct out_unit *u = &p.v[i];
                char *path = path_concat(outdir, u->out);
                if (write_file(path, u->text) < 0) {
                        errf("cannot write %s", path);
                        free(path);
                        return 1;
                }
                free(path);

                for (size_t k = 0; k < u->wants.n; k++) {
                        char *d = xasprintf("%s/%s.wants", outdir, u->wants.v[k]);
                        if (mkdir_p(d) < 0) {
                                errf("cannot create %s", d);
                                free(d);
                                return 1;
                        }
                        char *lp = path_concat(d, u->out);
                        unlink(lp);
                        if (symlink_relative(u->out, lp) < 0) {
                                errf("cannot link %s", lp);
                                free(lp);
                                free(d);
                                return 1;
                        }
                        free(lp);
                        free(d);
                }
                for (size_t k = 0; k < u->requires.n; k++) {
                        char *d = xasprintf("%s/%s.requires", outdir, u->requires.v[k]);
                        if (mkdir_p(d) < 0) {
                                errf("cannot create %s", d);
                                free(d);
                                return 1;
                        }
                        char *lp = path_concat(d, u->out);
                        unlink(lp);
                        if (symlink_relative(u->out, lp) < 0) {
                                errf("cannot link %s", lp);
                                free(lp);
                                free(d);
                                return 1;
                        }
                        free(lp);
                        free(d);
                }
        }

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
        return 0;
}

static int cmd_list(struct opts *o) {
        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        printf("%slocal stratum:%s %s%s%s\n", oc(BOLD), oc(RST),
               oc(CYA), cfg.init ? cfg.init : "(unset)", oc(RST));
        printf("\n%s%s%s\n", oc(BOLD), "explicit entries:", oc(RST));
        for (size_t i = 0; i < cfg.n_units; i++)
                printf("  %s%s%s/%s\n", oc(CYA), cfg.units[i].stratum, oc(RST),
                       cfg.units[i].name);

        printf("\n%splan (%zu unit%s):%s\n", oc(BOLD), p.n, p.n == 1 ? "" : "s", oc(RST));
        for (size_t i = 0; i < p.n; i++)
                printf("  %s%s%s/%s %s->%s %s%s%s\n",
                       oc(CYA), p.v[i].stratum, oc(RST), p.v[i].src,
                       oc(BOLD), oc(RST),
                       oc(GRN), p.v[i].out, oc(RST));

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
        return r < 0 ? 1 : 0;
}

static int cmd_tree(struct opts *o) {
        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        for (size_t i = 0; i < p.n; i++) {
                printf("%s%s%s %s(%s/%s)%s\n", oc(BOLD), oc(GRN), p.v[i].out,
                       oc(RST), p.v[i].stratum, p.v[i].src, oc(RST));
                for (size_t k = 0; k < p.v[i].edges.n; k++)
                        printf("  %s\n", p.v[i].edges.v[k]);
                for (size_t k = 0; k < p.v[i].wants.n; k++)
                        printf("  %s[Install]%s -> %s%s.wants%s\n", oc(BOLD), oc(RST),
                               oc(CYA), p.v[i].wants.v[k], oc(RST));
                for (size_t k = 0; k < p.v[i].requires.n; k++)
                        printf("  %s[Install]%s -> %s%s.requires%s\n", oc(BOLD), oc(RST),
                               oc(CYA), p.v[i].requires.v[k], oc(RST));
        }

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
        return r < 0 ? 1 : 0;
}

static int cmd_check(struct opts *o) {
        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
        return r < 0 ? 1 : 0;
}

static int cmd_setup(struct opts *o, char **args, int nargs) {
        if (nargs < 2) {
                usage();
                return 2;
        }
        const char *stratum = args[0], *unit = args[1];

        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }
        if (!cfg.init || !*cfg.init) {
                errf("[resolve] init is not set in %s", o->config);
                config_free(&cfg);
                return 1;
        }
        if (strcmp(stratum, cfg.init) == 0) {
                errf("%s is the local stratum, nothing to port", stratum);
                config_free(&cfg);
                return 1;
        }

        config_add(&cfg, stratum, unit);

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        if (r < 0 && !o->force) {
                errf("analysis failed, config not saved");
                config_free(&cfg);
                world_free(&w);
                sv_free(&strata);
                plan_free(&p);
                return 1;
        }

        if (config_save(&cfg) < 0)
                errf("cannot save %s", o->config);

        for (size_t i = 0; i < p.n; i++)
                printf("  %s%s%s/%s %s->%s %s%s%s\n",
                       oc(CYA), p.v[i].stratum, oc(RST), p.v[i].src,
                       oc(BOLD), oc(RST), oc(GRN), p.v[i].out, oc(RST));

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
        return 0;
}

static int cmp_str(const void *a, const void *b) {
        return strcmp(*(char *const *)a, *(char *const *)b);
}

static void collect_enabled(const struct config *cfg, const char *strata_root,
                            const char *stratum, strvec *out,
                            strvec *skipped_targets, strvec *ignored) {
        char *base = path_abs(strata_root);
        char *dir = xasprintf("%s/%s/etc/systemd/system", base, stratum);
        free(base);

        DIR *dp = opendir(dir);
        if (!dp) {
                free(dir);
                return;
        }

        struct dirent *de;
        while ((de = readdir(dp))) {
                if (de->d_name[0] == '.')
                        continue;
                if (!has_suffix(de->d_name, ".wants") && !has_suffix(de->d_name, ".requires"))
                        continue;

                char *wd = path_concat(dir, de->d_name);
                DIR *dp2 = opendir(wd);
                if (!dp2) {
                        free(wd);
                        continue;
                }
                struct dirent *e;
                while ((e = readdir(dp2))) {
                        if (e->d_name[0] == '.')
                                continue;
                        if (!is_unit_name(e->d_name))
                                continue;
                        if (has_suffix(e->d_name, ".target")) {
                                if (!sv_contains(skipped_targets, e->d_name))
                                        sv_push(skipped_targets, e->d_name);
                                continue;
                        }

                        int skip = 0;
                        for (size_t k = 0; k < cfg->ignore.n; k++)
                                if (fnmatch(cfg->ignore.v[k], e->d_name, 0) == 0)
                                        skip = 1;
                        if (skip) {
                                if (!sv_contains(ignored, e->d_name))
                                        sv_push(ignored, e->d_name);
                                continue;
                        }

                        if (!sv_contains(out, e->d_name))
                                sv_push(out, e->d_name);
                }
                closedir(dp2);
                free(wd);
        }
        closedir(dp);
        free(dir);
}

static void filter_available(const char *strata_root, const char *stratum,
                             strvec *found, strvec *dangling) {
        strvec names = {0};
        sv_push(&names, stratum);

        struct world w = {0};
        char *err = NULL;
        if (world_init(&w, strata_root, &names, &err) < 0) {
                free(err);
                sv_free(&names);
                return;
        }

        struct stratum_idx *s = world_get(&w, stratum);
        for (size_t i = 0; i < found->n; i++) {
                struct uent *e = NULL;
                int r = s ? sidx_resolve(s, found->v[i], &e) : -1;
                if (r <= 0) {
                        sv_push(dangling, found->v[i]);
                        found->v[i][0] = 0;
                }
        }

        size_t out = 0;
        for (size_t i = 0; i < found->n; i++) {
                if (found->v[i][0])
                        found->v[out++] = found->v[i];
                else
                        free(found->v[i]);
        }
        found->n = out;

        world_free(&w);
        sv_free(&names);
}

static int cmd_import(struct opts *o, char **args, int nargs) {
        if (nargs < 1) {
                usage();
                return 2;
        }
        const char *stratum = args[0];

        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }
        if (normalize_init(o, &cfg) < 0) {
                config_free(&cfg);
                return 1;
        }
        if (!cfg.init || !*cfg.init) {
                errf("[resolve] init is not set");
                config_free(&cfg);
                return 1;
        }
        if (strcmp(stratum, cfg.init) == 0) {
                errf("%s is the local stratum, nothing to import", stratum);
                config_free(&cfg);
                return 1;
        }

        strvec found = {0}, targets = {0}, dangling = {0}, ignored = {0};
        collect_enabled(&cfg, o->strata_root, stratum, &found, &targets, &ignored);
        filter_available(o->strata_root, stratum, &found, &dangling);
        if (found.n > 1)
                qsort(found.v, found.n, sizeof(char *), cmp_str);

        size_t before = cfg.n_units;
        for (size_t i = 0; i < found.n; i++)
                config_add(&cfg, stratum, found.v[i]);
        size_t added = cfg.n_units - before;

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        if (r < 0 && !o->force) {
                errf("analysis failed, config not saved");
                config_free(&cfg);
                world_free(&w);
                sv_free(&strata);
                plan_free(&p);
                sv_free(&found);
                sv_free(&targets);
                sv_free(&dangling);
        sv_free(&ignored);
                return 1;
        }

        if (!o->dry && config_save(&cfg) < 0) {
                errf("cannot save %s", o->config);
                config_free(&cfg);
                world_free(&w);
                sv_free(&strata);
                plan_free(&p);
                sv_free(&found);
                sv_free(&targets);
                sv_free(&dangling);
        sv_free(&ignored);
                return 1;
        }

        printf("%s%s%s: %s%zu%s enabled unit%s found, %s%zu%s new entr%s%s\n",
               oc(CYA), stratum, oc(RST),
               oc(BOLD), found.n, oc(RST), found.n == 1 ? "" : "s",
               oc(BOLD), added, oc(RST), added == 1 ? "y" : "ies",
               o->dry ? " (dry run, not saved)" : "");
        for (size_t i = 0; i < found.n; i++)
                printf("  %s\n", found.v[i]);
        if (targets.n) {
                printf("%sskipped %zu target link%s (targets are never ported):%s\n",
                       oc(YEL), targets.n, targets.n == 1 ? "" : "s", oc(RST));
                for (size_t i = 0; i < targets.n; i++)
                        printf("  %s\n", targets.v[i]);
        }
        if (dangling.n) {
                printf("%sskipped %zu dangling link%s (no unit file in %s):%s\n",
                       oc(YEL), dangling.n, dangling.n == 1 ? "" : "s", stratum, oc(RST));
                for (size_t i = 0; i < dangling.n; i++)
                        printf("  %s\n", dangling.v[i]);
        }
        if (ignored.n) {
                printf("%sskipped %zu ignored link%s (matches [resolve] ignore):%s\n",
                       oc(YEL), ignored.n, ignored.n == 1 ? "" : "s", oc(RST));
                for (size_t i = 0; i < ignored.n; i++)
                        printf("  %s\n", ignored.v[i]);
        }

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
        sv_free(&found);
        sv_free(&targets);
        sv_free(&dangling);
        sv_free(&ignored);
        return 0;
}

static int cmd_delete(struct opts *o, char **args, int nargs) {
        if (nargs < 2) {
                usage();
                return 2;
        }
        const char *stratum = args[0], *unit = args[1];

        struct config cfg;
        char *err = NULL;
        if (config_load(&cfg, o->config, &err) < 0) {
                errf("%s", err ? err : "config load failed");
                free(err);
                return 1;
        }
        if (config_del(&cfg, stratum, unit) < 0) {
                errf("%s/%s is not an explicit entry", stratum, unit);
                config_free(&cfg);
                return 1;
        }
        if (config_save(&cfg) < 0) {
                errf("cannot save %s", o->config);
                config_free(&cfg);
                return 1;
        }

        config_free(&cfg);
        return 0;
}

int main(int argc, char **argv) {
        struct opts o = {
                .config = "/bedrock/etc/global-units.conf",
                .strata_root = "/bedrock/strata",
        };

        use_color_out = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
        use_color_err = isatty(STDERR_FILENO) && !getenv("NO_COLOR");

        char **pos = xmalloc((size_t)argc * sizeof(*pos));
        int np = 0;

        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
                        o.config = argv[++i];
                else if (strcmp(argv[i], "--strata-root") == 0 && i + 1 < argc)
                        o.strata_root = argv[++i];
                else if (strcmp(argv[i], "--local") == 0 && i + 1 < argc)
                        o.local = argv[++i];
                else if (strcmp(argv[i], "--force") == 0)
                        o.force = 1;
                else if (strcmp(argv[i], "--no-reload") == 0)
                        o.no_reload = 1;
                else if (strcmp(argv[i], "--dry-run") == 0)
                        o.dry = 1;
                else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                        usage();
                        return 0;
                } else if (argv[i][0] == '-' && argv[i][1] != 0) {
                        errf("unknown option %s", argv[i]);
                        return 2;
                } else
                        pos[np++] = argv[i];
        }

        if (np == 0) {
                usage();
                return 2;
        }

        const char *cmd = pos[0];
        char **args = pos + 1;
        int nargs = np - 1;

        /* systemd invokes generators with three directory arguments and no
         * subcommand; the first one is an absolute path. */
        if (cmd[0] == '/') {
                use_color_out = 0;
                use_color_err = 0;
                return cmd_generate(&o, pos, np);
        }

        if (strcmp(cmd, "generate") == 0)
                return cmd_generate(&o, args, nargs);
        if (strcmp(cmd, "list") == 0)
                return cmd_list(&o);
        if (strcmp(cmd, "tree") == 0)
                return cmd_tree(&o);
        if (strcmp(cmd, "check") == 0)
                return cmd_check(&o);
        if (strcmp(cmd, "setup") == 0)
                return cmd_setup(&o, args, nargs);
        if (strcmp(cmd, "import") == 0)
                return cmd_import(&o, args, nargs);
        if (strcmp(cmd, "delete") == 0)
                return cmd_delete(&o, args, nargs);

        errf("unknown command %s", cmd);
        usage();
        return 2;
}
