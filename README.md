# systemd-cross

Offline prototype for porting foreign stratum systemd units into the
init-providing stratum's systemd instance, on Bedrock Linux.

This repository is intentionally **not** wired into `brl` yet. It only
implements the two pieces needed to develop and test the logic offline:

- a config editor/validator (`setup`, `delete`, `list`, `tree`, `check`)
- a unit generator (`generate <dir>`), shaped to be called as a systemd
  generator later

## Status

Early prototype. The generated units are written in a form that
`systemd-analyze verify --root=` accepts, and the dependency/closure
resolution works against fixture strata. Nothing is installed into a
live systemd yet.

## Model

```
config file  ->  plan (resolve + rewrite)  ->  unit files in <dir>
```

The config file is the only place where human decisions live. It never
contains derived state: closure members, provider bindings and final
unit names are recomputed on every run.

### Config format

Line based INI. Unknown keys are hard errors.

```ini
version = 1

[resolve]
init = auto
local_only = multi-user.target, network-online.target, *.mount, *.device, systemd-*
order = gentoo, debian

[stratum debian]
units = ssh.service
units = apt-daily.timer
```

- `init` — the stratum providing PID 1 (units there are native, never ported);
  `auto` detects it from `$BRL_INIT_STRATUM` or `/bedrock/strata/init`
- `local_only` — fnmatch patterns that always bind to the local stratum
- `order` — fallback provider order for dependencies not provided by the
  consumer's own stratum or the local one
- `[stratum X]` / `units` — explicit whitelist entries

### Resolution rules

For a dependency name `N` of a unit from stratum `S`:

1. `N` matches `local_only` -> local
2. `N` exists in `S` -> port it (prefix with `bedrock-<S>-`)
3. `N` exists in the local stratum -> local
4. first match in `order` -> port it
5. otherwise -> error

Masked units resolve to "missing" and never fall back.

Closure keys: `Requires`, `Wants`, `Requisite`, `BindsTo`, `Upholds`.
Ordering keys are rewritten but do not pull units in: `After`, `Before`,
`Conflicts`, `PartOf`, `OnFailure`, `OnSuccess`, ...

### Rewriting

| Directive | Action |
|---|---|
| `ExecStart*`, `ExecStop*`, `ExecReload*`, `ExecCondition` | prefix `/bedrock/bin/strat -r <stratum>` |
| dependency lists | provider-resolved, foreign names prefixed |
| `EnvironmentFile=` | absolute paths qualified with the stratum root |
| `Description=` | prefixed with `[<stratum>] ` |
| `[Install] WantedBy/RequiredBy` | local targets only; links emitted into `<target>.wants/` |
| `RootDirectory=`, `RootImage=`, `DynamicUser=yes`, `PrivateUsers=yes` | fatal, unit dropped |

Units are emitted under `bedrock-<stratum>-<original-name>` so that
implicit socket/timer pairing keeps working.

## Usage

The build is statically linked by default (`LDFLAGS = -static`) so the
binary runs under any init stratum, including musl ones.

```sh
make
./systemd-cross --config tests/test.conf --strata-root tests/fixtures/strata check
./systemd-cross --config tests/test.conf --strata-root tests/fixtures/strata tree
./systemd-cross --config tests/test.conf --strata-root tests/fixtures/strata generate /tmp/out
```

`--strata-root` exists for offline testing; the default is
`/bedrock/strata`. `--config` defaults to
`/bedrock/etc/global-units.conf`.

Generator mode: the first argument is the output directory. Under
systemd the generator is invoked with three directories; only the first
is used.

## Testing

```sh
make test
```

Fixtures live in `tests/fixtures/strata/{alpha,beta}`. The test suite
checks closure, dependency rewriting, Exec wrapping, path
qualification, install links, mask/alias/template handling, and runs
`systemd-analyze verify --root=` on the produced units.

## Deployment

```text
/bedrock/libexec/systemd-cross                        static binary
/bedrock/libexec/systemd-generators/systemd-cross     symlink, generator dir
/bedrock/etc/global-units.conf                        config
```

The bedrock init script exports, just before handing off to the real init:

```sh
export SYSTEMD_GENERATOR_PATH="/bedrock/libexec/systemd-generators:"
export BRL_INIT_STRATUM="${init_stratum}"
```

The trailing `:` keeps systemd's compiled-in generator search path. This makes
the generator available regardless of which stratum provides PID 1, without
relying on any stratum-private `/usr/local/lib`.

## Known limitations (v0.1)

- `ExecStart=@...` (argv0 override) is not translated
- `[Install] Alias=` / `Also=` are not emitted
- foreign `[Install]` targets are skipped with a warning (targets are not ported)
- `WorkingDirectory=` outside shared paths only warns
- `%n` / `%N` change meaning after renaming (not rewritten)
- `systemctl link` units (`LINKED`) are read but not treated specially
- mount-sandbox directives (`ProtectSystem=`, `PrivateTmp=`, ...) are passed
  through unchanged; they apply to the host namespace, so their effect on the
  chrooted payload is weaker than intended
- `Type=dbus` units depend on the init stratum's system bus policy
- no `brl` integration, no live systemd installation
