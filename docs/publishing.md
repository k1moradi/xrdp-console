# Publishing checklist

The repository is a standalone GPLv3 project. It publishes the upstream xrdp
version and archive hash together with the patch series needed to reproduce
the direct-Console daemon. The generated source, private binaries, credentials,
Xauthority cookies, systemd backups, and raw benchmark logs are not published.

Work directly on `main`; this project does not create release or publication
branches. Set a real Git identity and verify that the remote is the intended
`k1moradi/xrdp-console` repository. Do not infer a repository name from this
workspace directory:

```sh
git config user.name "Your Name"
git config user.email "you@example.com"
git remote -v
git fetch origin --prune
git status --short
git switch main
git diff --cached --check
git commit -m "Publish direct-X11 xrdp-console"
git push -u origin main
```

If the remote has existing history that is not present in this working copy,
reconcile it on `main` before publishing. A push to an unrelated repository is
not a valid publication of this project.

The GitHub Actions workflow builds the portable Release configuration, runs
CTest, and creates both CPack artifacts. It does not run the live-display
benchmark because CI has no physical X11 console or Microsoft RDP client.

For a release, update `project(VERSION ...)` in `CMakeLists.txt`, run the
maintainer workflow in `docs/maintainers.md`, and tag the commit:

```sh
git tag -a v0.1.0 -m "xrdp-console 0.1.0"
git push origin v0.1.0
```
