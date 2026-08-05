# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**M0–M8 complete, 2026-07-28.** The application is functional on Linux: it opens and indexes a log, filters and highlights it, tails it live, splits it into runs, restores its session, and autodetects a format. Directory layout is `src/core/` (UI-free, links QtCore only), `src/ui/` (Qt Widgets), and `tests/`; the commands below work and all CTest cases pass.

**M9 complete:** several logs open at once as tabs in a central document area; the side panes are the only dock widgets and cannot be dragged into it (`ARCHITECTURE.md` §12.2).

**M10 complete:** highlight rules match on the same five axes as filters — subsystem, thread, priority, time range and message text — through one shared `MatchCriteria`/`FilterSet` predicate and one shared `AxisEditor` widget. See `PLAN.md` M10.

**M11 complete:** logs on other machines open over SSH, written `ssh://user@host/path` and accepted anywhere a path is. A remote log is fetched forward into a local spool and read back through an ordinary local source, so the paint path is unchanged and tailing works exactly as it does locally (`ARCHITECTURE.md` §6.3). libssh2 is the project's first non-Qt dependency and is **optional** — auto-detected, never required; without it everything still builds and a remote open explains that support is not compiled in.

**M12 complete:** compressed and archived logs open directly — `.gz .bz2 .xz .zst`, zip, tar and every compressed tar — as a **second fetcher behind the same spool** M11 built (`ARCHITECTURE.md` §6.4). An archive is a file *type* and SSH is a *transport*, so the two compose: the archive fetcher's input is an ordinary `LogSource`, which for a remote container is the SSH spool, and `ssh://host/var/log/app.log.1.gz` chains the two with neither knowing the other exists. Addresses continue through the container (`bundle.tar.gz/var/log/app.log`), so no new scheme and no session schema bump. Expansion is ordinary live ingestion — records appear as they expand — and then the stream genuinely ends: `LogSource::isComplete()` (non-pure, false by default, exactly the route `wasReplaced()` took) stops the watch, with no user-facing mode and the follow control untouched. libarchive is optional and auto-detected, gated exactly as libssh2 is.

**libarchive, unlike libssh2, IS exercised in CI.** The archive layer needs no network, no credentials and no disposable remote path: `tst_archivefetcher`, `tst_archivetail`, `tst_archivemembers` and `tst_archiveopen` build real `.gz`/`.xz`/`.zip`/`.tar.gz` files at runtime with libarchive's own write side (`tests/ArchiveFixtures.h`), and nothing binary is committed. `tst_archivelocation` and `tst_complete` are **ungated** and run in every configuration, because the path layer and the completion contract must behave identically with and without the dependency. The caveat below about M11 does not carry over to M12.

**Nothing in CI exercises libssh2 beyond linking.** The transport's own paths — handshake, host-key verification, agent/keyboard-interactive auth, and whether a real `sftp-server`'s FSTAT follows the handle — are covered only by `tst_sshlive`, which skips unless `LOFTAIL_TEST_SSH_URL` names a disposable remote path. Everything above `RemoteFetcher` is covered without a network by `tst_spooledsource`, `tst_remotetail` and `tst_remoteopen`, using `tests/FakeFetcher.h`. When changing the SSH layer, run `tst_sshlive` against a real host by hand; a green pipeline means nothing about it.

**M13 complete:** a log that is not there is a **state, not an error** (`SPEC.md` §3 "Logs that are not there", `ARCHITECTURE.md` §6.5). A path that does not exist yet opens a waiting tab and starts reading when the log turns up; a log deleted while open clears to the same state and comes back when it does; an unreachable SSH host opens and reconnects in the background. Waiting lives on the **live seam**, not in a fourth `LogSource` — what changes is not how bytes are read but whether there are any yet, which is `LiveController`'s existing question. The one asymmetry to keep straight: a waiting **local** document releases its source, a waiting **spooled** one keeps it, because the source owns the spool and the spool owns the fetcher that is retrying. Only genuine refusals still fail to open — a malformed address, an archive naming no member, a changed host key, a rejected password, a dependency not built in.

**Two M13 details that are easy to undo by accident.** `originVanished()` is checked **after** `wasReplaced()`, and only after a **2 s grace period measured in elapsed time, not in checks** — the filesystem watcher fires checks too, and a rotation produces a burst of them at exactly the moment the path is briefly empty, so a count-based grace period would blank the view during a `logrotate`. And `BufferedLogSource::originVanished()` deliberately does **not** go through `pathIdentity()`, which is stubbed to 0 on Windows and would report every open file as vanished on the first tick.

**Outstanding regardless of milestone:** Windows and macOS builds and runtime behavior are still unverified (`PLAN.md` M6/M7), including the stubbed Windows `BufferedLogSource` share-mode open and `pathIdentity()`.

**Windows testing has no fonts.** The offscreen QPA plugin on Windows uses Qt's own font database, which looks in `$QTDIR/lib/fonts`; Qt no longer ships fonts, so `QFontDatabase::families()` comes back **empty** and nothing resolves — not even to a wrong font. (Offscreen on Linux has fontconfig, so this never shows up locally.) Any test that asserts on resolved font properties must guard on an empty family list and `QSKIP`; `tst_logview::everyColumnRendersFixedPitch` does. This is an environment limit, not a `monospaceFont()` bug.

**Windows headers define `min` and `max` as macros.** `std::numeric_limits<qint64>::min()` — `Record::kNoTimestamp`, `FilterSet`'s time bounds — becomes a syntax error in any translation unit that reaches `<windows.h>` before a loftail header. Nothing here includes it directly; **libarchive's public header does**, so the breakage lands on include *order* and only on Windows, which is exactly the rule nobody remembers. The build defines `NOMINMAX` (and `WIN32_LEAN_AND_MEAN`) PUBLIC on `loftail_core` so `loftail_ui` and every test inherit it. This bit M12 in CI, in two test files that included the fixture header first.

**When a Windows test fails, read the CI diagnostic, not the ctest output.** Test output does not reach ctest on Windows — a failure shows as `***Failed` with an empty block, which reads like a crash and is not. The Windows job re-runs each failed binary one test function at a time, prints per-function exit codes, and dumps QtTest's report via `-o file,txt` for the failing one. That is where the actual assertion appears.

## What loftail is

A cross-platform (Windows/macOS/Linux) desktop GUI viewer for logs produced by **log4cplus**. Every file is opened watched for new content, so it serves finished logs and still-being-written logs with no mode switch (`SPEC.md` §3). It filters and highlights by subsystem (the logger name passed to `Logger::getInstance()`) and priority, with saved presets and full session restore.

## Document map

| File              | Contents                                                                  |
| ----------------- | ------------------------------------------------------------------------- |
| `SPEC.md`         | User-visible behavior of what has shipped. The product definition.        |
| `FUTURE.md`       | User-visible features planned for **later** releases, and the P1 accommodations that keep them cheap to add. |
| `ARCHITECTURE.md` | Internal technical decisions and rationale. Not user-visible.             |
| `PLAN.md`         | Milestone-by-milestone implementation plan.                               |

When a change alters user-visible behavior, update `SPEC.md`. When it alters an internal contract or invariant, update `ARCHITECTURE.md`. Keep product decisions out of `ARCHITECTURE.md` and implementation detail out of `SPEC.md`.

## Stack

- **C++20**, **Qt 6** (6.10 is installed on the dev machine; target 6.4 minimum — the version in Ubuntu 24.04's repos)
- **Qt Widgets**, not QML — see `ARCHITECTURE.md` for the rationale; the decision is load-bearing and should not be revisited casually
- **CMake ≥ 3.21** + **Ninja**
- **libssh2**, optional and auto-detected, for remote logs (M11). The build must keep working without it — configure prints `SSH remote sources: ENABLED/DISABLED`. `-DLOFTAIL_WITH_SSH=OFF` forces it off; `-DLOFTAIL_SSH_FETCH=ON` builds it from source (used by the Windows CI job, which has no package manager)
- **libarchive**, optional and auto-detected, for compressed and archived logs (M12). Configure prints `Archived log sources: ENABLED/DISABLED` and `-DLOFTAIL_WITH_ARCHIVE=OFF` forces it off. **It has no build-from-source fallback, unlike `LOFTAIL_SSH_FETCH`, and deliberately so:** libarchive is only useful *with* its codecs, and it locates zlib and liblzma through `find_package` in **MODULE** mode, which `CMAKE_FIND_PACKAGE_REDIRECTS_DIR` does not reach — so a FetchContent-built codec is silently ignored, libarchive builds with no compression at all, and still reports itself found. That failure is invisible until a `.gz` refuses to open. Get it from a package manager (`libarchive-dev`, `brew install libarchive`, `vcpkg install libarchive`); the Windows CI job uses vcpkg. A **static** libarchive additionally needs `-DLOFTAIL_ARCHIVE_STATIC=ON`, or its `__declspec(dllimport)` API links against import stubs that do not exist
- **Qt Test** for unit tests
- **Reference build environment is Ubuntu 24.04 LTS**: the project must build with the stock toolchain (GCC 13, CMake 3.28, Ninja, Qt 6.4.2) using no separately-installed Qt. This is why the Qt minimum is 6.4, not 6.5 LTS — see `ARCHITECTURE.md` §1.

loftail does **not** link log4cplus. It reads log files as text; it has no compile-time relationship to the library that produced them.

## Commands

```bash
# Configure (Debug)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Run
./build/src/loftail

# All tests
ctest --test-dir build --output-on-failure

# A single test binary
./build/tests/tst_patterncompiler

# A single test case within a binary (Qt Test convention)
./build/tests/tst_sessiongui sessionRoundTrip
```

On Windows and macOS, `CMAKE_PREFIX_PATH` must point at the Qt installation.

## Architecture invariants

These ten constraints are cheap to honor from the start and expensive to retrofit. Do not violate them without explicitly reopening the decision.

**loftail does not link log4cplus, and links exactly two other non-Qt libraries** — libssh2 and libarchive — both optional, both auto-detected, and the build must stay green without either.

1. **The model stores byte offsets, never parsed text.** A record entry is `{offset, timestamp, length, loggerId, threadId, lineCount, priority}` — exactly 32 bytes. Message text is parsed lazily inside `data()`. Holding parsed strings for every record makes large files unusable.

2. **A record is not a line.** log4cplus messages can contain embedded newlines, so one record may span several physical lines. Indexing rule: a line matching `recordStartRe` begins a record; non-matching lines are continuations of the preceding record. Code that assumes one-line-per-row is wrong.

3. **Nothing downstream of the parser knows about the pattern string.** `PatternCompiler` turns a log4cplus `ConversionPattern` into a `LogFormat` (regex + field map). Views, filters, and highlighters consume only the field map. This indirection is what makes format autodetection (a later-release feature, `FUTURE.md`) a drop-in rather than a rewrite.

4. **Filtering compares integers, not strings.** Logger and thread names are interned to `quint32` ids at index time; priority is an enum **declared in severity order** so filtering by minimum level is one `>=` test (`ARCHITECTURE.md` §7.2); timestamps are `qint64`. Filter predicates operate on those. The intern tables double as the subsystem and thread lists shown in the filter pane. Message-text filtering is the one axis with no integer fast path, so it runs last in the predicate chain — including on the paint path, where highlight rules use the same axes and the same lazy-decode ordering.

5. **Every file is opened append-aware; there is no post-mortem vs live mode.** loftail cannot know whether a file is still being written, so all files are watched and a finished log is just one that never grows (`SPEC.md` §3). No `LogSource` may assume the file is immutable, and none may hold it in a way that blocks the writer from appending, rotating, or truncating — observing a log must not disturb the process producing it. File access goes through the `LogSource` interface; the model cannot tell which implementation it has — local or remote. The mmap (POSIX) vs buffered (Windows) split is platform-driven, not mode-driven (`ARCHITECTURE.md` §6); a remote or archived log reads through a local spool that one of those two then backs (§6.3, §6.4). Rotation is asked of the source via `wasReplaced()`, never worked out by the caller: what has to be re-resolved differs per source. **The one permitted exception, added in M12:** a source may declare its stream finished via `isComplete()` — but only where loftail *produced* the bytes itself from a fixed input and can prove there are no more, i.e. an expanded archive member. The ban is on *guessing* that a file somebody else is writing has stopped. Nothing user-facing follows from it: the watch stops because there is no work left, the follow control is untouched, and there is still nothing to turn on or forget.

**Not an exception at all, added in M13:** a source may report that its origin is *gone* via `originVanished()`. That is an **observation, not a guess** — "the file is not there" is what a stat answers, which is exactly why `isComplete()` needs loftail to have produced the bytes and this does not. The document then *waits* rather than erroring (`SPEC.md` §3, §6.5), and the watch keeps running because waiting is the one state in which it is the only thing making progress.

6. **The record table is a custom `LogView : QAbstractScrollArea`, not a `QTableView`.** Multi-line records render at full height, and `QTableView` cannot do variable row heights lazily — it needs an O(n) `resizeRowsToContents()` pass or per-row entries in `QHeaderView`, either of which defeats the lazy index. `LogView` scrolls in *line* units over two-level prefix sums of `Record::lineCount`. It has two geometry modes: **exact** (wrap off / selected-record-only) and **estimated** (wrap always on, where height depends on viewport width). Keep the estimation machinery unreachable from the exact path. See `ARCHITECTURE.md` §7.1–7.1.1; this is the project's highest-risk component.

7. **All per-file state lives in `Document`; all per-view state lives in `DocumentView`; nothing reaches for "the current file" globally.** Several files are open at once and one file may have several views, so the two scopes must not blur: filters, highlighters, format and run selection belong to the *file*; scroll, selection, wrap mode, column layout, follow and find belong to the *view*. No singletons holding file state; panes bind to an `activeDocumentChanged(Document*)` signal rather than a fixed reference, and that signal fires only when the **file** changes, never when switching between two views of one file. The settings schema stores a `documents` array and a `views` array. See `ARCHITECTURE.md` §12.

8. **Never scan for `\n` in raw bytes.** Encoding is user-selectable, defaults to auto-detect, and may be UTF-16, where a newline is `0A 00` or `00 0A`. All line-boundary and text work goes through the `Decoder` layer; only `Record::offset`/`length` stay in byte terms. See `ARCHITECTURE.md` §6.1 — this is the easiest invariant to violate by accident.

9. **The indexer is a single forward pass.** No backward passes, no seek-and-re-read. This is the constraint that let SSH-backed sources ship in M11 as an addition rather than a rewrite: because the indexer never seeks backwards, a remote log's spool can be filled and indexed *at the same time*, which is the whole difference between following a remote log and downloading one. **It paid a second time in M12**, and more visibly: an archived log fills in *as it expands* rather than freezing until it is done, for exactly the same reason. Two features, one constraint — while `isRandomAccess()`, the flag actually designed for this job, remains unread by any branch in the codebase. Random access is fine in `data()` on the paint path, which only touches already-indexed records. See `ARCHITECTURE.md` §6.2–§6.4.

10. **`Record::timestamp` is always UTC epoch milliseconds.** The source time zone and the timestamp display mode are both user-configurable, but conversion happens exactly twice: source zone applied at index time, display zone applied when formatting or interpreting typed filter bounds — or not at all, in the two *seconds* display modes, which render the stored ms directly. Nothing in between is zone-aware. Storing local wall-clock time would make comparisons zone-dependent and break across DST transitions, where the same local time occurs twice. See `ARCHITECTURE.md` §5.1.

## Conventions

- Qt naming throughout (`camelCase` members, `PascalCase` types) so the codebase reads consistently with the framework it sits on.
- Prefer `QStringView`/`QByteArrayView` on parse paths; avoid allocating per record.
- No hardcoded paths — `QStandardPaths::AppConfigLocation` for settings and presets.
- Open log files in binary mode and handle CRLF explicitly rather than relying on platform text-mode translation.
- Parsing and indexing logic must be unit-testable without a `QApplication` instance; keep it free of UI dependencies.

## Working notes

- The user refers to the logging library as "cplus4log"; the actual library is **log4cplus**. Confirmed 2026-07-20.
- Format configuration was manual first (user supplies the `ConversionPattern`); autodetection was deliberately built after it, in M8, behind the `IFormatProvider` seam. Both paths still end at the same `PatternCompiler`, and a detected pattern is never applied without confirmation — it pre-fills the Log Format dialog (`SPEC.md` §4).
