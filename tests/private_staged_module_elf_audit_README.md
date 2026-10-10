# Private staged-module ELF static audit

This is a **read-only, non-authorizing** companion to the guarded
`xrdp-console-private-stage` build target in PR #45. It checks the actual
first-party module after native build/staging using system `readelf` and
SHA-256. It never invokes `ldd`, `dlopen`, xrdp, chansrv, X11, Firefox,
FreeRDP or clipboard APIs.

## Why

A module can be copied successfully into the correct matched private
`<install>/lib/xrdp/` directory and have the same SHA-256 as the built
artifact, yet its **DT_RUNPATH or DT_NEEDED** may resolve a private library
from another install or protected build tree. File staging alone cannot
establish the static dynamic-loader search topology.

The audit requires a canonical caller-owned `.release/xrdp-console`
root, an existing hash-keyed private `xrdp-install-<16-hex>` directory,
a distinct built `libxrdp_console.so` in a build area under that root,
and the staged `<install>/lib/xrdp/libxrdp_console.so`. The hashes must
exactly match one explicit 64-character SHA-256.

It then inspects only staged module bytes using system `readelf -W -h`
and `readelf -W -d`; the ELF must be a shared object with expected
first-party SONAME if present. All DT_RUNPATH/RPATH entries must resolve
**inside that same private installation**. Required private
`libxrdp.so*` and `libcommon.so*` DT_NEEDED names must each resolve
unambiguously through those search directories to regular files inside
the matched `lib/xrdp` directory. Internal libtool versioned symlinks
are allowed when their *canonical targets* remain private.

Any external build prefix, system `/usr/local` path, protected install,
relative/empty search path, unknown variable expansion, symlink escape,
duplicate RUNPATH directory, missing private dependency, module swap,
or changed bytes is a hard audit error.

The report lists unresolved system DT_NEEDED entries but **does not**
claim they are available at runtime. It also cannot prove source-to-ELF
provenance without the build transcript, linker symbol compatibility,
`dlopen` resolution, `LD_LIBRARY_PATH` behavior, sesman/chansrv IPC,
X11 requestor identity or Firefox PNG File acceptance.

## Host operator command shape (not executed here)

After a *separately authorized build-only* validation successfully
creates and stages the actual private module:

```sh
python3 -B tests/private_staged_module_elf_audit.py \
  --release "$REL" \
  --install "$INSTALL" \
  --built "$REL/build/abc-private-verified/bin/libxrdp_console.so" \
  --staged "$INSTALL/lib/xrdp/libxrdp_console.so" \
  --sha256 "<sha256-of-actual-built-module>"
```

This script returns exit code **2** even on a successful static inspection,
with `runtime_authorized=false`. Inspect its JSON `mode` and error fields;
do not treat a successful audit as permission to start a session.

## Offline regression

`xrdp-private-staged-module-elf-unit` registers inert ELF-like bytes
and mocks system readelf. It covers exact stage byte matching, private
libtool SONAME symlinks, external/protected library redirects, RPATH and
RUNPATH escapes, duplicate paths/tags, incorrect module name/SONAME,
unsupported ELF type, missing library links and permanent runtime NO-GO.
It launches no xrdp or X11 binary.

Source baseline is corrected chansrv PR #32 → private CMake PR #40 →
isolated compiler PR #42 → provenance PR #43 → module staging PR #45 →
this static ELF audit.
