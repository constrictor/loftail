# Packaging loftail

This directory holds everything needed to turn a build of loftail into a
distributable artifact on each platform. The goal (PLAN.md M7) is one artifact
per platform that **runs on a machine with no Qt development environment
installed**.

The Qt runtime is bundled into every artifact by the platform's standard deploy
tool — `linuxdeploy` on Linux, `windeployqt` on Windows, `macdeployqt` on macOS.
loftail does not link log4cplus, but it does link two optional libraries — libssh2
(remote logs, M11) and libarchive (compressed and archived logs, M12) — so "nothing
else to bundle" is no longer true. On Linux `linuxdeploy` resolves them through `ldd`
and copies them in without a script change; CI asserts both are present in the
AppImage, because a `--version` smoke test never opens a remote or archived log and a
missing library would surface only for a user. On Windows both are built statically
from source, so there is nothing extra for `windeployqt` or the portable zip to carry
— which is the reason they are static there.

**The packaging scripts configure their own build tree**, so flags given to a tested
build do not reach the artifact. On Linux that is harmless: the dependencies come from
apt and are auto-detected either way, and CI asserts both `.so`s are in the AppImage.
On Windows they must be passed explicitly (`build-portable.ps1 -CMakeArgs …`), and CI
asserts the *packaged* configure reported both `ENABLED` — because the failure mode is
a zip that ships without a feature every test in the same run just proved works.

## The licence travels with the binary

loftail is GPL-3.0-or-later, so **every artifact must carry the full licence text** —
GPLv3 §4 asks for it and a Linux distribution will reject a package without it. Where it
lands, and who puts it there:

| Artifact | Path inside it | Placed by |
| --- | --- | --- |
| `.deb` | `/usr/share/doc/loftail/LICENSE` and `.../copyright` | the `install()` rules in `src/CMakeLists.txt`, which CPack packs |
| AppImage | the same two, under `usr/share/doc/loftail/` | the same rules — `build-appimage.sh` stages the AppDir with `cmake --install` |
| Windows zip | `LICENSE.txt` beside `loftail.exe` | an explicit `Copy-Item` in `build-portable.ps1`, before `Compress-Archive` |
| macOS `.app`/`.dmg` | `Contents/Resources/LICENSE` | an explicit `cp` in `build-appbundle.sh`, **before** `macdeployqt -dmg` |

Only the two Linux artifacts get it for free, because they are the only ones with an
install layout to hang it off; the Windows and macOS rules install the bare binary. So a
new artifact type needs a line of its own — and the failure is silent, since a zip with
no licence in it builds, runs and passes every test.

`/usr/share/doc/loftail/copyright` is `packaging/linux/copyright`, in Debian's
machine-readable DEP-5 format. That exact path is what `lintian`'s
`copyright-file-missing` looks for, which is why the file is not merely a second copy of
`LICENSE`: it also names the licences of the libraries the package links.

## Command line

All platforms share the same CLI (finalized in M7, `src/main.cpp`):

```
loftail [options] [file]

  file            Log file to open (optional). Opens at its end and follows it,
                  like tail -f — this is unconditional (SPEC.md §3), which is why
                  there is deliberately no --follow flag.
  --pattern <p>   log4cplus ConversionPattern for the files named. It overrides
                  whatever is remembered for them, and is then checked against
                  each: where it fits it is remembered for that log; where it
                  does not, Preferences opens to correct it, and dismissing that
                  leaves the log unopened and nothing saved.
  -h, --help      Show usage and exit.
  -v, --version   Show version and exit.
```

No file argument opens an empty window (session restore may still reopen the
last file).

## File association — soft (capability), never the default

loftail advertises that it **can** open `.log` files, so it appears in the OS
"Open With" list — but it never registers itself as the **default** handler for
`.log` (or any) files, on any platform. These are two independent layers, and the
app only ever touches the first:

| | Declared by the app | Effect |
| --- | --- | --- |
| **Capability ("soft")** | Linux `MimeType=text/x-log;` · macOS `LSHandlerRank=Alternate` | listed under "Open With" |
| **Default ("hard")** | *not* in the app — `xdg-mime default`, `duti`, or the user picking "always open with" | opens on double-click |

Declaring the capability never steals the existing default (`less`, a text editor,
etc.); becoming the default stays a user/installer decision (PLAN.md M7).
Accordingly:

- the Linux `.desktop` file declares `MimeType=text/x-log;` — the freedesktop type
  `*.log` resolves to (a subclass of `text/plain`), scoped so loftail does **not**
  volunteer for every text file. It takes effect only once XDG desktop integration
  installs the `.desktop` file (`appimaged` / AppImageLauncher / a distro package);
  a bare AppImage run from `~/Downloads` registers nothing.
- the macOS `Info.plist` declares a `CFBundleDocumentTypes` entry for the `log`
  extension with `CFBundleTypeRole=Viewer` and `LSHandlerRank=Alternate` (the
  "Open With, not default" rank).
- the Windows portable zip still registers nothing; associations there belong in an
  MSI/NSIS installer if wanted.

An installer (NSIS/MSI/`.deb`) — or the user — is still the place to make loftail
the **default** handler if wanted.

## Linux — AppImage (reference environment: Ubuntu 24.04)

Ubuntu 24.04 LTS is the reference build environment (ARCHITECTURE.md §1): stock
GCC 13 + CMake + Ninja + Qt 6.4.2 from the system repos, no separately-installed
Qt.

```bash
packaging/linux/build-appimage.sh          # Release by default
```

What it does:

1. downloads (and caches under `packaging/linux/tools/`) `linuxdeploy` and
   `linuxdeploy-plugin-qt` from their upstream GitHub releases,
2. configures + builds loftail,
3. `cmake --install`s into a staging `AppDir` (the `install()` rules in
   `src/CMakeLists.txt` place the binary in `usr/bin`, the `.desktop` file in
   `usr/share/applications`, and the icon in `usr/share/icons/...`),
4. runs `linuxdeploy --plugin qt` to pull the Qt libraries + platform plugins
   into the AppDir and emit `dist/loftail-0.1.0-x86_64.AppImage`.

For an offline build, drop the two `linuxdeploy*.AppImage` tools into
`packaging/linux/tools/` yourself; the script only downloads what is missing.

**Clean-machine verify:** copy the `.AppImage` to a machine (or container) with
no Qt installed and run it. Headless smoke check:

```bash
./loftail-0.1.0-x86_64.AppImage --version
QT_QPA_PLATFORM=offscreen ./loftail-0.1.0-x86_64.AppImage some.log --pattern '%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n'
```

A GUI launch should show the window with no `libQt6*.so` on the system.

## Linux — .deb (Ubuntu 24.04 and 26.04)

The distro package is the **opposite artifact to the AppImage**, and both exist on
purpose. The AppImage bundles Qt so it runs on any distribution; the `.deb` links
the distribution's own Qt, so it is ~500 KB instead of ~50 MB and picks up Qt
security updates from the archive rather than from a re-release of loftail.

```bash
cmake -S . -B build-deb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-deb
cd build-deb && cpack -G DEB     # → loftail_0.1.0~ubuntu24.04_amd64.deb
```

Configuration lives in `packaging/linux/deb.cmake`, included from the top-level
`CMakeLists.txt` after the subdirectories so CPack packs the same `install()` rules
that stage the AppDir. There is no second install layout.

**One package per Ubuntu release, and this is not optional.** 24.04 has Qt 6.4.2 and
26.04 a much newer Qt with different sonames, so no single `.deb` can satisfy both.
The release is read from `/etc/os-release` and stamped into the version —
`0.1.0~ubuntu24.04` — using `~`, which sorts *before* the plain version in dpkg
ordering, so a per-release build never shadows an upstream `0.1.0` on upgrade. Pass
`-DLOFTAIL_DEB_DISTRO=ubuntu24.04` when building in a container, where the host's
`/etc/os-release` is not the target's.

**`Depends:` is derived, never written by hand** (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`).
Hand-listing would be wrong twice: the Qt package names differ between releases, and
libssh2/libarchive are optional — a build configured without them must not produce a
package demanding them. `dpkg-shlibdeps` reads what was actually linked, so the
dependency list follows the configuration for free:

```
Depends: libarchive13t64 (>= 3.2.1), libc6 (>= 2.34), libgcc-s1 (>= 3.0),
         libqt6core6t64 (>= 6.10.2), libqt6gui6 (>= 6.9.1), libqt6network6 (>= 6.1.2),
         libqt6widgets6 (>= 6.4.0), libssh2-1t64 (>= 1.2.9), libstdc++6 (>= 5)
```

The corollary is what CI has to check. The AppImage step asserts libssh2 and
libarchive are *inside* the artifact; the `.deb` bundles nothing, so the same
guarantee becomes an assertion that both appear in `Depends` — because
`dpkg-shlibdeps` writes a thin, entirely valid-looking `Depends` just as happily when
an optional library was auto-detected off. CI also asserts the inverse, that no
`libQt6*.so` is inside the package, since a bundled Qt here would be an unmanaged,
unpatched copy.

Installing the package is also what makes the **soft file association** real: unlike
a bare AppImage run from `~/Downloads`, a `.deb` puts `loftail.desktop` in
`/usr/share/applications`, so loftail appears under "Open With" for `.log` files. It
still never becomes the default handler.

**Clean-machine verify:** `sudo apt-get install ./loftail_*.deb` on a machine with no
Qt *development* environment — apt resolving `Depends` is the actual test, which is
why CI installs through apt rather than `dpkg -i`.

## Windows — portable zip (+ optional installer)

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\build-portable.ps1 `
    -QtDir C:\Qt\6.4.2\msvc2019_64
```

Builds Release, installs the exe, runs `windeployqt` to copy the Qt DLLs +
plugins next to it, and produces `dist\loftail-0.1.0-windows-x64.zip`. Unzip
anywhere and run `loftail.exe`.

An MSI/NSIS installer can wrap the same staged tree (and is the right place for
Start-menu shortcuts and file association, if wanted). The portable zip is the
baseline and needs no installer.

**Clean-machine verify:** unzip on a Windows machine with no Qt installed and
run `loftail.exe` (and `loftail.exe --version`).

> Status: this script is authored but has **not** been run or verified on
> Windows from this Linux dev environment. The CI workflow
> (`.github/workflows/packaging.yml`) is the vehicle for actually building and
> smoke-testing it.

## macOS — .app bundle (+ .dmg)

```bash
packaging/macos/build-appbundle.sh
```

Builds Release as a `MACOSX_BUNDLE`, runs `macdeployqt` to copy the Qt
frameworks into the bundle, and wraps it in `dist/loftail-0.1.0-macos.dmg`.

**Signing / notarization:** the baseline bundle is **unsigned**. Distributed as
is, Gatekeeper warns users and they must right-click ▸ Open (or
`xattr -dr com.apple.quarantine loftail.app`). For real distribution, set
`CODESIGN_IDENTITY` to a Developer ID and notarize the `.dmg` with `notarytool`.

**Clean-machine verify:** copy the `.app`/`.dmg` to a Mac with no Qt installed
and launch it (`open loftail.app`, or `loftail.app/Contents/MacOS/loftail
--version`).

> Status: this script is authored but has **not** been run or verified on macOS
> from this Linux dev environment. The CI workflow is the vehicle for actually
> building it.

## Continuous integration

`.github/workflows/packaging.yml` builds, tests, and packages loftail on Linux,
Windows, and macOS. It is the mechanism by which the Windows and macOS artifacts
— which cannot be produced from the Linux dev machine — get built and
smoke-tested, and it doubles as the "verify the build on Windows and macOS"
check that M0 left open.

The `deb` job is a matrix over Ubuntu 24.04 and 26.04. It carries a second job
beyond the package: ARCHITECTURE.md §1 pins the Qt **floor** at 6.4 and every other
Linux build honors it, so the 26.04 leg is the only thing in CI that compiles
loftail against a much newer Qt and GCC — the **ceiling** nothing else tests. It
runs the full suite for that reason, not only `cpack`.

`ubuntu-26.04` is a **public preview** runner image (announced 2026-06-11): available
to everyone, but with tool versions that still move. That leg started non-blocking and
is now a required check like the rest — it has run green with no preview-side trouble,
and since it is the only job compiling against a Qt newer than the floor, letting it
fail quietly would leave the ceiling untested at exactly the moment it broke.

## Releasing

**A release promotes a build; it does not make one.** `.github/workflows/release.yml`
takes the run ID of a `build-test-package` run that has already gone green, downloads
that run's artifacts, tags the commit it was built from, and publishes those exact
files. It never compiles anything.

That is deliberate, and the reason is in this directory. `build-appimage.sh` fetches
linuxdeploy and its Qt plugin from their upstream **`continuous`** tag, the Windows job
installs libarchive through vcpkg at build time, and the Linux jobs take Qt from
whatever the runner image currently carries. Rebuilding a tagged commit a week later
therefore produces *different bytes* — bundled by different tooling, linked against
different libraries — that no one has smoke-tested. Promoting ships the artifact whose
green tick you actually looked at.

`packaging.yml` consequently has **no tag trigger**, and should not regain one: the tag
is applied to a commit that workflow already built, so building again on the tag would
produce artifacts nothing publishes while suggesting they are what ships.

The cycle, with the version bumped *after* the release:

1. `master` already reads, say, `project(VERSION 0.2.0)`. Every build off it reports
   `loftail 0.2.0+<run>.g<sha>` — recognisably a build heading for 0.2.0, never
   confusable with released 0.2.0.
2. Pick a green run and note its ID (the number in its URL).
3. Actions ▸ **release** ▸ Run workflow, with that run ID and the tag `v0.2.0`. It
   defaults to a **draft** so the generated notes can be read before they go out.
4. Publish the draft.
5. Bump `project(VERSION)` to `0.3.0` and commit. `master` is now heading for 0.3.0.

**The version comes from `CMakeLists.txt`, never from the tag.** The tag only has to
agree with it, and the workflow checks that rather than trusting it: it runs the
downloaded AppImage's `--version` and requires both halves to match — the release
against the tag, and the build id against the run being promoted. So the classic
mistake, tagging `v0.3.0` on a tree still saying `0.2.0`, fails the promote instead of
mislabelling a release. Deriving the version from the tag instead would leave every
build from source claiming no version at all, which ARCHITECTURE.md §1 rules out.

Four things worth knowing before the first run:

- **Only a successful `push`-to-`master` run can be promoted.** A `pull_request` run
  builds a throwaway merge commit that exists on no branch and is garbage-collected;
  tagging one would point the release at a commit nobody can check out.
- **Artifacts expire after 90 days** (the GitHub maximum for a public repository), so
  that is how long a run stays promotable. Nothing sets `retention-days:` today, which
  means the default — raise it there if the release cadence ever approaches it.
- **The tag is created before the release**, because a *draft* release does not create
  its tag: GitHub defers that until the draft is published, which would leave the
  promoted commit unmarked for as long as the draft sat there. The trade is that
  abandoning a draft leaves the tag behind, and deleting it is a manual step.
- **`release.yml` is unexercised until it is first used.** Its guards and its version
  parsing were driven against real run JSON and fabricated artifacts before it shipped,
  but nothing in CI runs it — publishing a release is not a thing a test may do.
