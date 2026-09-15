# CLAUDE.md: Debian packaging

This is guidance for Claude Code on the `debian` branch. The upstream guidance
(code, tests, conventions) is in `../docs/CLAUDE.md`; this file covers only
packaging. Human-readable build and update instructions are in
`README.source`.

## Branch model

- `main` is the platform-agnostic upstream branch. Never add `debian/`, distro
  CI or distro-specific text there.
- `debian` is `main` plus `debian/` plus `.github/workflows/release-deb.yml`.
  Bring upstream changes in with `git merge main`. Merges no longer try to
  delete the packaging, because the first merge already recorded keeping it.
- Don't modify upstream files on this branch. The format is `3.0 (quilt)` with
  no patches. A change outside `debian/` would show up as an unrepresentable
  diff, except under `.github/`, which `debian/source/options` ignores. Make
  upstream changes on `main` instead.

## Files

| File | Purpose |
|---|---|
| `control` | source `quick-question`, binary `quick-question` (Section utils); Build-Depends debhelper-compat 13, libcjson-dev, libcurl4-openssl-dev, `python3 <!nocheck>` |
| `rules` | plain `dh` with `hardening=+all`; `override_dh_auto_install` runs `make install DESTDIR=… PREFIX=/usr INSTALL_STRIP=` so that dh_strip builds the -dbgsym package |
| `changelog` | version `<upstream>-<revision>`; the distribution is an Ubuntu series (`resolute`) |
| `copyright` | DEP-5, Expat (MIT), 2026 Ian Lamont |
| `source/format`, `source/options` | `3.0 (quilt)`; `extend-diff-ignore` for `.github/` |
| `docs`, `examples` | install `README.md` and `config.example.json` |
| `quick-question.lintian-overrides` | `initial-upload-closes-no-bugs`: not uploaded to the Debian archive, so there's no ITP bug. Add overrides only with a comment giving the reason. |
| `README.source` | branch model, updating, building, CI |
| `.gitignore` | debhelper build output |

## Building and checking

```sh
dpkg-buildpackage -us -uc -b            # runs make test; output in ../
lintian -I --pedantic ../quick-question_*_amd64.changes
dpkg-deb -c ../quick-question_*_amd64.deb
```

A packaging change is done only when all of these hold:
- The build succeeds, with the unit and integration tests passing inside it.
- Plain `lintian` prints nothing. With `-I --pedantic`, only these known tags
  remain:
  - `debian-watch-file-is-missing`: upstream publishes no release tarballs to
    watch yet.
  - `redundant-priority-optional-field` and
    `redundant-rules-requires-root-no-field`: kept on purpose for older build
    environments such as the 24.04 CI runner.
- The `.deb` contains:
  - `/usr/bin/qq` (stripped)
  - `/usr/share/man/man1/qq.1.gz`
  - `/usr/share/doc/quick-question/` with `README.md.gz`,
    `changelog.Debian.gz`, `copyright` and `examples/config.example.json`
  - `/usr/share/lintian/overrides/quick-question`

For a source package, also build once with the orig tarball (see
`README.source`). `dpkg-source` must accept the tree without patches.

To test without cluttering the parent directory, build in a copy made with
`git ls-files -co --exclude-standard`.

## Versioning

- For a new upstream release: merge `main`, then add a changelog entry
  `<QQ_VERSION>-1`.
- For a packaging-only change: bump the revision (`-2`, `-3`, …).
- The workflow fails if the upstream part of the changelog version differs
  from `QQ_VERSION` in `src/qq.h`.

## CI and releases

`.github/workflows/release-deb.yml` runs on pushes to `debian`, on
`ubuntu-24.04`, with `contents: write`. Runs are serialized by a `concurrency`
group. The workflow:
1. Checks the versions.
2. Runs `dpkg-buildpackage -us -uc -b`.
3. Runs lintian on the `.deb`, failing on errors only.
4. Publishes a GitHub release with the runner's preinstalled `gh`.
   - The release is tagged `debian/<version>` on the pushed commit and titled
     `quick-question <version> (Debian package)`, with the `.deb` attached
     and notes built from the changelog entry.
   - If that release already exists, the step only logs a notice. Never
     replace a published `.deb`; bump the changelog revision instead.

The README on `main` tells users to download these releases. Keep the asset
name `quick-question_<version>_amd64.deb`.

Keep the package buildable on the runner: 24.04 ships cJSON 1.7.17, curl
8.5.0 and debhelper 13.14. Don't require newer build dependencies.
