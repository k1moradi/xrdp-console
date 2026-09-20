# Publishing checklist

The repository is a standalone GPLv3 project. It publishes the patched xrdp
source needed to reproduce the optimized shared-console daemon under
`third_party/xrdp-0.10.6.1-optimized`, together with its license notices. It
does not publish private binaries, credentials, Xauthority cookies, systemd
backups, or raw benchmark logs.

Work directly on `main`; this project does not create release or publication
branches. Set a real Git identity and verify that the remote is the intended
xrdp-x11vnc repository. Do not infer a repository name from this workspace
directory:

```sh
git config user.name "Your Name"
git config user.email "you@example.com"
git remote -v
git fetch origin --prune
git status --short
git switch main
git diff --cached --check -- . ':(exclude)third_party'
git commit -m "Publish optimized xrdp and x11vnc benchmark toolkit"
git push -u origin main
```

If the remote has existing history that is not present in this working copy,
reconcile it on `main` before publishing. A push to an unrelated repository is
not a valid publication of this project.

The GitHub Actions workflow builds the portable Release configuration, runs
CTest, and creates both CPack artifacts. It does not run the live-display
benchmark because CI has no physical X11 console, x11vnc password, or RDP
client display.

For a release, update `project(VERSION ...)` in `CMakeLists.txt`, run the
maintainer workflow in `docs/maintainers.md`, and tag the commit:

```sh
git tag -a v0.1.0 -m "xrdp-console 0.1.0"
git push origin v0.1.0
```
