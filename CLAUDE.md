# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**Pre-implementation.** As of 2026-07-20 this repository contains specification documents only — no source, no build system, no git repository. The stack is decided (see below); nothing is scaffolded yet. The build commands in this file describe the intended layout and will not work until Milestone M0 of `PLAN.md` is complete. Update this section once it is.

## What loftail is

A cross-platform (Windows/macOS/Linux) desktop GUI viewer for logs produced by **log4cplus**, supporting both post-mortem inspection and live tailing. It filters and highlights by subsystem (the logger name passed to `Logger::getInstance()`) and priority, with saved presets and full session restore.

## Document map

| File | Contents |
|---|---|
| `SPEC.md` | User-visible behavior only. The product definition. Supersedes `idea.md`. |
| `ARCHITECTURE.md` | Internal technical decisions and rationale. Not user-visible. |
| `PLAN.md` | Milestone-by-milestone implementation plan. |

When a change alters user-visible behavior, update `SPEC.md`. When it alters an internal contract or invariant, update `ARCHITECTURE.md`. Keep product decisions out of `ARCHITECTURE.md` and implementation detail out of `SPEC.md`.

## Stack

- **C++20**, **Qt 6** (6.10 is installed on the dev machine; target 6.5 LTS minimum)
- **Qt Widgets**, not QML — see `ARCHITECTURE.md` for the rationale; the decision is load-bearing and should not be revisited casually
- **CMake ≥ 3.21** + **Ninja**
- **Qt Test** for unit tests

loftail does **not** link log4cplus. It reads log files as text; it has no compile-time relationship to the library that produced them.

## Commands (intended — valid after M0)

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
./build/tests/tst_patterncompiler testPaddingModifier
```

On Windows and macOS, `CMAKE_PREFIX_PATH` must point at the Qt installation.

## Architecture invariants

These nine constraints are cheap to honor from the start and expensive to retrofit. Do not violate them without explicitly reopening the decision.

1. **The model stores byte offsets, never parsed text.** A record entry is `{offset, timestamp, length, loggerId, threadId, lineCount, priority}` — exactly 32 bytes. Message text is parsed lazily inside `data()`. Holding parsed strings for every record makes large files unusable.

2. **A record is not a line.** log4cplus messages can contain embedded newlines, so one record may span several physical lines. Indexing rule: a line matching `recordStartRe` begins a record; non-matching lines are continuations of the preceding record. Code that assumes one-line-per-row is wrong.

3. **Nothing downstream of the parser knows about the pattern string.** `PatternCompiler` turns a log4cplus `ConversionPattern` into a `LogFormat` (regex + field map). Views, filters, and highlighters consume only the field map. This indirection is what makes format autodetection (P2) a drop-in rather than a rewrite.

4. **Filtering compares integers, not strings.** Logger names are interned to `quint32` ids at index time; priorities are an enum. Filter predicates operate on those. The intern table doubles as the subsystem list shown in the filter pane.

5. **File access goes through the `LogSource` interface.** Post-mortem uses mmap; live tail uses buffered incremental reads. The model must not be able to tell which. This exists because mmap semantics for growing files differ meaningfully on Windows — see `ARCHITECTURE.md`.

6. **The record table is a custom `LogView : QAbstractScrollArea`, not a `QTableView`.** Multi-line records render at full height, and `QTableView` cannot do variable row heights lazily — it needs an O(n) `resizeRowsToContents()` pass or per-row entries in `QHeaderView`, either of which defeats the lazy index. `LogView` scrolls in *line* units over two-level prefix sums of `Record::lineCount`. It has two geometry modes: **exact** (wrap off / selected-record-only) and **estimated** (wrap always on, where height depends on viewport width). Keep the estimation machinery unreachable from the exact path. See `ARCHITECTURE.md` §7.1–7.1.1; this is the project's highest-risk component.

7. **All per-file state lives in `Document`; nothing reaches for "the current file" globally.** Multiple open files are deferred but must stay reachable: no singletons holding file state, panes bind to an `activeDocumentChanged(Document*)` signal rather than a fixed reference, and the settings schema stores a `documents` array even while it always has one element. See `ARCHITECTURE.md` §12.

8. **Never scan for `\n` in raw bytes.** Encoding is auto-detected and may be UTF-16, where a newline is `0A 00` or `00 0A`. All line-boundary and text work goes through the `Decoder` layer; only `Record::offset`/`length` stay in byte terms. See `ARCHITECTURE.md` §6.1 — this is the easiest invariant to violate by accident.

9. **The indexer is a single forward pass.** No backward passes, no seek-and-re-read. Compressed and SSH-backed sources are planned (`SPEC.md` §11) and neither supports random access during indexing. Random access is fine in `data()` on the paint path, which only touches already-indexed records. See `ARCHITECTURE.md` §6.2.

## Conventions

- Qt naming throughout (`camelCase` members, `PascalCase` types) so the codebase reads consistently with the framework it sits on.
- Prefer `QStringView`/`QByteArrayView` on parse paths; avoid allocating per record.
- No hardcoded paths — `QStandardPaths::AppConfigLocation` for settings and presets.
- Open log files in binary mode and handle CRLF explicitly rather than relying on platform text-mode translation.
- Parsing and indexing logic must be unit-testable without a `QApplication` instance; keep it free of UI dependencies.

## Working notes

- The user refers to the logging library as "cplus4log"; the actual library is **log4cplus**. Confirmed 2026-07-20.
- Format configuration is manual in P1 (user supplies the `ConversionPattern`). Autodetection is P2 and is planned for but deliberately not built first — see `PLAN.md` M8 and the `IFormatProvider` seam.
- The original `idea.md` sketch was superseded by `SPEC.md` and deleted on 2026-07-20 at the user's request.
