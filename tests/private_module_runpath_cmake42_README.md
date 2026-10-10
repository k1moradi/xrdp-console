# Private module RUNPATH and CMake 4.2 cache compatibility

This source-only fix responds to the *first matched native build* report for
PR #45. Native compile/link, 52/52 patch replay, 217/217 upstream unit tests,
and module staging succeeded, but **runtime is still NO-GO** because the
staged `libxrdp_console.so` had an empty final DT_RUNPATH component.

## Link-time correction

`src/CMakeLists.txt` now uses a private-only function in
`cmake/private_module_rpath.cmake` to set:

- `BUILD_WITH_INSTALL_RPATH TRUE`
- `INSTALL_RPATH "$ORIGIN"`
- `INSTALL_RPATH_USE_LINK_PATH FALSE`
- `BUILD_RPATH ""`

The existing normal build's `BUILD_RPATH` and `INSTALL_RPATH` properties
are **unchanged**. Only the opt-in private xrdp branch uses the new function.

Private module staging copies the built `libxrdp_console.so` byte-for-byte
into `<private-install>/lib/xrdp`, rather than performing a CMake
install-time ELF RPATH rewrite. Setting its final loader-relative
`$ORIGIN` search path during the actual link avoids reserving/padding an
unused install RPATH string that can produce an empty DT_RUNPATH component.

No `patchelf`, in-place ELF edit, global linker override or
`LD_LIBRARY_PATH` workaround is performed.

`tests/test_private_module_linker_rpath.py` **really configures and links a
synthetic C shared module** using exactly this helper and system CMake/CC.
It inspects the synthetic module with `readelf -W -d`, requiring precisely
one `$ORIGIN` RUNPATH, no empty components, and an actual DT_NEEDED for
a synthetic shared dependency. It does not compile or execute real xrdp,
invoke X11, connect RDP, or load the module.

A host native rebuild is required to verify the real module's linker
command, actual DT_NEEDED, final DT_RUNPATH, byte hashes, and compatibility
with GNU libtool's private libraries. The separate PR #47 staged-module
ELF auditor still rejects empty RUNPATH components and private library
escapes; its return code remains non-authorizing even on a valid report.

## CMake 4.2.3 cache metadata

CMake may generate:

`CMAKE_FIND_PACKAGE_REDIRECTS_DIR:STATIC=<current-build>/CMakeFiles/pkgRedirects`

The provenance parser accepts **only this exact redirects STATIC key**
with an owned, canonical directory at the verified build's
`CMakeFiles/pkgRedirects`, rejecting missing, linked, traversing, protected,
or external paths. The Linux CMake 4.2.3 native build additionally generated
`CMAKE_PROJECT_COMPAT_VERSION:STATIC=` because the top-level `project()`
does not specify `COMPAT_VERSION`. The parser allows **exactly an empty**
`CMAKE_PROJECT_COMPAT_VERSION:STATIC` value and refuses a nonempty value,
other cache types or arbitrary extra STATIC keys. The check also applies to
directly inspected cache mappings, so callers cannot bypass the parser.
Older CMake versions without either generated entry remain supported.

The tool still independently checks the real Git ancestry, 52 ordered
patches, build-state hash, owned private source/dependency paths, archive
hash, and restricted compiler inputs. As before, it always exits 2 with
`runtime_authorized=false`; JSON content distinguishes a successful
static configuration check from an error.

## Operator scope

The host agent must read the exact new PR head, preserve existing
artifacts/reports, and perform only a separately authorized private
**build-only** revalidation in a new, isolated release directory.
Do not attempt full CTest, xrdp/chansrv execution, private Xvfb/Firefox,
clipboard, CLIPRDR peer, system install, production service action, merge,
or deployment.

A clean native module RUNPATH and passing static ELF/provenance audits
are necessary but **not sufficient** for a C-leg runtime session or real
macOS screenshot-to-Firefox PNG File acceptance.
