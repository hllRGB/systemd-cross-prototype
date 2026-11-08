#define _GNU_SOURCE
#include "config.h"
#include "index.h"
#include "plan.h"

#include <errno.h>
#include <fcntl.h>
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
};

static void usage(void) {
        fprintf(stderr,
                "usage: systemd-cross [options] <command> [args]\n"
                "\n"
                "commands:\n"
                "  setup <stratum> <unit>   add an explicit port entry to the config\n"
                "  delete <stratum> <unit>  remove an explicit port entry\n"
                "  list                     show configured units and their plan status\n"
                "  tree                     show resolved dependency edges\n"
                "  check                    analyse only, no writes\n"
                "  generate <dir>           generator mode: write units into <dir>\n"
                "\n"
                "options:\n"
                "  --config <path>       config file (default /bedrock/etc/foreign-units.conf)\n"
                "  --strata-root <path>  strata root (default /bedrock/strata)\n"
                "  --local <stratum>     override [resolve] init\n"
                "  --force               save even if analysis failed\n");
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
                        fprintf(stderr, "error: %s\n", p->errors.v[i]);
        }
        for (size_t i = 0; i < p->warns.n; i++)
                fprintf(stderr, "warning: %s\n", p->warns.v[i]);
}

static int analyse(struct opts *o, struct config *cfg, struct plan *p,
                   struct world *w, strvec *strata) {
        memset(p, 0, sizeof *p);
        collect_strata(cfg, strata);
        char *err = NULL;
        if (world_init(w, o->strata_root, strata, &err) < 0) {
                fprintf(stderr, "error: %s\n", err ? err : "world init failed");
                free(err);
                return -1;
        }
        if (o->local)
                cfg->init = xstrdup(o->local);
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
                fprintf(stderr, "error: %s\n", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);
        if (r < 0) {
                fprintf(stderr, "error: not generating any unit\n");
                config_free(&cfg);
                world_free(&w);
                sv_free(&strata);
                plan_free(&p);
                return 1;
        }

        if (mkdir_p(outdir) < 0) {
                fprintf(stderr, "error: cannot create %s\n", outdir);
                return 1;
        }

        for (size_t i = 0; i < p.n; i++) {
                struct out_unit *u = &p.v[i];
                char *path = path_concat(outdir, u->out);
                if (write_file(path, u->text) < 0) {
                        fprintf(stderr, "error: cannot write %s\n", path);
                        free(path);
                        return 1;
                }
                free(path);

                for (size_t k = 0; k < u->wants.n; k++) {
                        char *d = xasprintf("%s/%s.wants", outdir, u->wants.v[k]);
                        if (mkdir_p(d) < 0) {
                                fprintf(stderr, "error: cannot create %s\n", d);
                                free(d);
                                return 1;
                        }
                        char *lp = path_concat(d, u->out);
                        unlink(lp);
                        if (symlink_relative(u->out, lp) < 0) {
                                fprintf(stderr, "error: cannot link %s\n", lp);
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
                                fprintf(stderr, "error: cannot create %s\n", d);
                                free(d);
                                return 1;
                        }
                        char *lp = path_concat(d, u->out);
                        unlink(lp);
                        if (symlink_relative(u->out, lp) < 0) {
                                fprintf(stderr, "error: cannot link %s\n", lp);
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
                fprintf(stderr, "error: %s\n", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        printf("local stratum: %s\n", cfg.init ? cfg.init : "(unset)");
        printf("\nexplicit entries:\n");
        for (size_t i = 0; i < cfg.n_units; i++)
                printf("  %s/%s\n", cfg.units[i].stratum, cfg.units[i].name);

        printf("\nplan (%zu unit%s):\n", p.n, p.n == 1 ? "" : "s");
        for (size_t i = 0; i < p.n; i++)
                printf("  %s/%s -> %s\n", p.v[i].stratum, p.v[i].src, p.v[i].out);

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
                fprintf(stderr, "error: %s\n", err ? err : "config load failed");
                free(err);
                return 1;
        }

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        for (size_t i = 0; i < p.n; i++) {
                printf("%s (%s/%s)\n", p.v[i].out, p.v[i].stratum, p.v[i].src);
                for (size_t k = 0; k < p.v[i].edges.n; k++)
                        printf("  %s\n", p.v[i].edges.v[k]);
                for (size_t k = 0; k < p.v[i].wants.n; k++)
                        printf("  [Install] -> %s.wants\n", p.v[i].wants.v[k]);
                for (size_t k = 0; k < p.v[i].requires.n; k++)
                        printf("  [Install] -> %s.requires\n", p.v[i].requires.v[k]);
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
                fprintf(stderr, "error: %s\n", err ? err : "config load failed");
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
                fprintf(stderr, "error: %s\n", err ? err : "config load failed");
                free(err);
                return 1;
        }
        if (!cfg.init || !*cfg.init) {
                fprintf(stderr, "error: [resolve] init is not set in %s\n", o->config);
                config_free(&cfg);
                return 1;
        }
        if (strcmp(stratum, cfg.init) == 0) {
                fprintf(stderr, "error: %s is the local stratum, nothing to port\n", stratum);
                config_free(&cfg);
                return 1;
        }

        config_add(&cfg, stratum, unit);

        struct world w = {0};
        struct plan p;
        strvec strata = {0};
        int r = analyse(o, &cfg, &p, &w, &strata);

        if (r < 0 && !o->force) {
                fprintf(stderr, "error: analysis failed, config not saved\n");
                config_free(&cfg);
                world_free(&w);
                sv_free(&strata);
                plan_free(&p);
                return 1;
        }

        if (config_save(&cfg) < 0)
                fprintf(stderr, "error: cannot save %s\n", o->config);

        for (size_t i = 0; i < p.n; i++)
                printf("%s/%s -> %s\n", p.v[i].stratum, p.v[i].src, p.v[i].out);

        config_free(&cfg);
        world_free(&w);
        sv_free(&strata);
        plan_free(&p);
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
                fprintf(stderr, "error: %s\n", err ? err : "config load failed");
                free(err);
                return 1;
        }
        if (config_del(&cfg, stratum, unit) < 0) {
                fprintf(stderr, "error: %s/%s is not an explicit entry\n", stratum, unit);
                config_free(&cfg);
                return 1;
        }
        if (config_save(&cfg) < 0) {
                fprintf(stderr, "error: cannot save %s\n", o->config);
                config_free(&cfg);
                return 1;
        }

        config_free(&cfg);
        return 0;
}

int main(int argc, char **argv) {
        struct opts o = {
                .config = "/bedrock/etc/foreign-units.conf",
                .strata_root = "/bedrock/strata",
        };

        int i = 1;
        for (; i < argc; i++) {
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
                else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                        usage();
                        return 0;
                } else if (argv[i][0] == '-') {
                        fprintf(stderr, "error: unknown option %s\n", argv[i]);
                        return 2;
                } else
                        break;
        }

        if (i >= argc) {
                usage();
                return 2;
        }

        const char *cmd = argv[i++];
        char **args = argv + i;
        int nargs = argc - i;

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
        if (strcmp(cmd, "delete") == 0)
                return cmd_delete(&o, args, nargs);

        fprintf(stderr, "error: unknown command %s\n", cmd);
        usage();
        return 2;
}
