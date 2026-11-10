# systemd-cross

Offline prototype for porting foreign stratum systemd units into the
init-providing stratum's systemd instance, on Bedrock Linux.

Though deployed for daily use on my machine, this repository is **not** ready for everyone yet. It only
implements the two pieces needed to develop and test the logic:

- a config editor/validator (`setup`, `delete`, `list`, `tree`, `check`)
- a unit generator (`generate <dir>`), shaped to be called as a systemd
  generator later

## Status

Early but usable prototype. The generated units are written in a form that
`systemd-analyze verify --root=` accepts, and the dependency/closure
resolution works against fixture strata.

## THINGS YOU MUST KNOW

You need extra modifications to make it work. See `## Deployment`

- Export the `SYSTEMD_GENERATOR_PATH`=`/bedrock/libexec/systemd-generators:` in bedrock's `/sbin/init` so the systemd can find the generator.
- Also export the `SYSTEMD_GENERATOR_PATH`=`/bedrock/libexec/systemd-user-generators:` to the environment through `/bedrock/etc/bedrock.conf` so the systemd-user can find it too.
- `ncstrat` ,the `strat` binary but has no capabilities is also required. Place it to `/bedrock/libexec/ncstrat`

It is only tested on my machine. Think twice or make a system snapshot (if you can) before really using this.


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

- `init` — the stratum providing PID 1; its units are native by default, but
  can still be explicitly ported (they keep `[Install]` and get no links);
  `auto` detects it from `$BRL_INIT_STRATUM` or `/bedrock/strata/init`
- `local_only` — fnmatch patterns that always bind to the local stratum
- `order` — fallback provider order for dependencies of foreign units not
  provided by the consumer's own stratum or the local one
- `ignore` — fnmatch patterns skipped by `import` (e.g. `bedrock-*`)
- `[stratum X]` / `units` — explicit whitelist entries

### Resolution rules

For a dependency name `N` of a unit from stratum `S`:

1. `N` matches `local_only` -> local
2. `S` is the local stratum -> local (local units are never re-bound)
3. `N` exists in `S` -> port it (prefix with `<S>-`)
4. `N` exists in the local stratum -> local
5. first match in `order` -> port it
6. otherwise -> error

A unit masked in a foreign consumer's own stratum resolves to "missing" and
does not fall back. A masked unit in a candidate provider stratum is skipped.
`local_only` entries missing from the local stratum only produce a warning.

Closure keys: `Requires`, `Wants`, `Requisite`, `BindsTo`, `Upholds`, plus
`Sockets=` and the `Unit=`/`Service=` links of timer/path/socket units.
Ordering keys are rewritten but do not pull units in: `After`, `Before`,
`Conflicts`, `PartOf`, `PropagatesReloadTo`, `ReloadPropagatedFrom`,
`JoinsNamespaceOf`, `OnFailure`, `OnSuccess`.

### Rewriting

| Directive | Action |
|---|---|
| `ExecStart*`, `ExecStop*`, `ExecReload*`, `ExecCondition` | prefix `/bedrock/bin/ncstrat <stratum>` |
| dependency lists | provider-resolved, foreign names prefixed |
| `EnvironmentFile=` | absolute non-shared paths qualified with the stratum root; leading `-` preserved |
| `Description=` | prefixed with `[<stratum>] ` unless already bracketed |
| `RequiresMountsFor=`, path-based `Condition*`/`Assert*` | absolute paths qualified with the stratum root |
| `CapabilityBoundingSet=`, `AmbientCapabilities=` | `CAP_SYS_CHROOT` added; error if explicitly excluded |
| `SystemCallFilter=` | `chroot` appended to positive allow-lists |
| `WorkingDirectory=` | warning when outside shared paths |
| `StandardOutput=file:`, `StandardError=file:` | warning, left unchanged |
| `[Install] WantedBy/RequiredBy` | local targets only; `.wants/`/`.requires/` links emitted for foreign strata only |
| `[Install] Alias=`, `Also=` | prefixed with `<stratum>-` |
| `RootDirectory=`, `RootImage=`, `RootImageOptions=`, `DynamicUser=yes/true`, `PrivateUsers=yes/true` | fatal, unit dropped |

Units are emitted as `<stratum>-<original-name>`. Companion services are
queued automatically: a `.timer`/`.path` pulls in its `.service` unless
`Unit=` is set, and a `.socket` pulls in its `.service` unless `Service=` is
set. Drop-ins are merged before rewriting, including template and prefix
(`foo-bar-.service.d`) variants; the highest-priority unit directory wins for
duplicate names. Every generated unit also gets `Conflicts=<original-name>`
so the ported copy and the native unit cannot run at the same time. Every
unit, including those from the init stratum, is rewritten and wrapped in
`ncstrat`, so the global name carries the provider stratum explicitly. Units
from the init stratum keep their `[Install]` stanza but get no wants/requires
links: the native unit already covers boot, and linking the copy would start
the service twice. A generated name that collides with a local unit is an
error.

## Usage

Two layers:

- `systemd-cross` — static C backend. Machine-readable commands:
  `plan`, `units [stratum]`, `locate <unit> [stratum]`,
  `import-list <stratum>`, `config-add`, `config-del`,
  `config-add-many` and `generate`; output is tab separated. The backend also
  implements `setup`, `delete`, `import <stratum>`, `list`, `tree` and
  `check`. Backend `import` scans `<stratum>/etc/systemd/system/*.wants` and
  `*.requires` for enabled units.
- `cli/brl-systemd` — busybox sh user interface, installed as
  `brl systemd`. Formatting and colours come from
  `/bedrock/share/common-code`.

Backend:

```sh
make
./systemd-cross plan
./systemd-cross import-list debian
./systemd-cross generate /tmp/out
```

CLI:

```sh
SYSTEMD_CROSS_BIN=$PWD/systemd-cross sh cli/brl-systemd \
    --config tests/test.conf --strata-root tests/fixtures/strata list
```

Options (`-c/--config`, `--strata-root`, `--local`, `--user`, `--from`,
`--sudo`, `--force`, `--dry-run`) may appear before or after the command.
Defaults are `/bedrock/etc/global-units.conf` and `/bedrock/strata`.

With `--user` and a non-root caller, config writes are performed through
`sudo` by default; `--sudo <cmd>` selects another elevation command (for
example `doas`).

`import <unit>...` locates each unit (init stratum first, then `order`, then
any other configured stratum) and adds exactly those entries. `--from
<stratum>` restricts the search. `setup` and `import` revert on analysis
failure unless `--force` is given.

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

## User scope

`--user` switches the tool to user-manager units:

- search roots per stratum: `etc/systemd/user`, `usr/local/share/systemd/user`,
  `usr/share/systemd/user`, `usr/local/lib/systemd/user`, `usr/lib/systemd/user`
- default config: `/bedrock/etc/global-units.user.conf`
- local-stratum user units are rewritten and wrapped like any other

Deployment uses a user generator directory and an environment variable
delivered to `systemd --user` through PAM:

```text
/bedrock/libexec/systemd-user-generators/systemd-cross   (symlink)
bedrock.conf [env-vars]:
    SYSTEMD_GENERATOR_PATH = /bedrock/libexec/systemd-user-generators:
```

`/etc/environment` is read by `pam_env` for `user@.service`, so the user
manager's own environment receives `SYSTEMD_GENERATOR_PATH` before it runs its
generators. No kernel command line changes are involved.

## Known limitations (v0.1)

- `ExecStart=@...` (argv0 override) is not translated
- `[Install] Alias=` / `Also=` prefix the whole value, so multi-value lists
  are only partially rewritten
- backend `import` always scans `etc/systemd/system`, even with `--user`
- foreign `[Install]` targets are skipped with a warning (targets are not ported)
- `WorkingDirectory=` outside shared paths only warns
- `StandardOutput=file:` / `StandardError=file:` only warn
- `%n` / `%N` change meaning after renaming (not rewritten)
- `systemctl link` units (`LINKED`) are read but not treated specially
- most mount-sandbox directives (`ProtectSystem=`, `PrivateTmp=`, ...) are
  passed through unchanged; they apply to the host namespace, so their effect
  on the chrooted payload is weaker than intended. Only
  `CapabilityBoundingSet=`, `AmbientCapabilities=` and positive
  `SystemCallFilter=` are adjusted so the wrapper can chroot
- `Type=dbus` units depend on the init stratum's system bus policy
- D-Bus activation breaks for ported units whose bus name exists only in a
  foreign stratum: crossfs exposes the foreign D-Bus activation file, but its
  `SystemdService=` still names the original unit, while the ported unit is
  `<stratum>-<name>`. D-Bus therefore cannot find the unit; crossfs must be
  changed to apply the same unit-name prefix (TODO).
