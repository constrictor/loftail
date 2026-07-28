# loftail

**A desktop viewer for [log4cplus](https://github.com/log4cplus/log4cplus) logs — every file opens live, like `tail -f`, with filters, highlighting, and full session restore.**

[![build-test-package](https://github.com/constrictor/loftail/actions/workflows/packaging.yml/badge.svg)](https://github.com/constrictor/loftail/actions/workflows/packaging.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt 6.4+](https://img.shields.io/badge/Qt-6.4%2B-41cd52)
![Windows | macOS | Linux](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)

---

## Why

Reading a log usually means picking a tool by how dead the file is: a pager for finished logs, `tail -f` for live ones. loftail refuses the choice. **Every file is opened watched**, at its end, following — a finished log is simply one that never grows ([`SPEC.md` §3](SPEC.md)). There is no mode to switch, and none to forget to turn on.

On top of that it does what a text editor can't: it knows the log's *shape*. Given the `ConversionPattern` your application used, loftail splits every record into fields and lets you filter and colorize by **subsystem** (the logger name you passed to `getInstance()`), **priority**, **thread**, **time range**, and **message text** — in a table where a stack trace stays a single row.

loftail does **not** link log4cplus. It reads log files as text and has no compile-time relationship to the library that produced them.

## Features

| | |
| --- | --- |
| 🔴 **Always live** | Files open at the end and follow. Rotation and truncation are detected and reloaded silently. Reading never blocks the writer from appending. |
| 🧩 **Format-aware** | Enter a log4cplus `ConversionPattern` and get a live preview of the fields it extracts — a wrong pattern is visible instantly, never silent. Remembered per file. |
| 🔮 **Autodetection** | New files are detected against a library of common patterns, with structural inference as a fallback. The guess pre-fills the dialog for confirmation — never applied silently. |
| 🪄 **Filters that scale** | Subsystem, minimum priority, thread, time range, message regex — OR within an axis, AND across axes. Each toggleable without deleting it. |
| 🎨 **Highlighting** | Ordered rules recolor background and/or text from a curated palette defined once for light themes and once for dark, so highlights stay legible in both. |
| 🧭 **Runs** | Give a regex that marks the start of an application run, and view one run at a time — scrolling, filtering, and find all restricted to it. |
| 🗂️ **Tabs** | Several logs open at once, as reorderable tabs in a document area the side panes can never invade — and a second view onto one log pins the history while the other keeps tailing. |
| 💾 **Session restore** | Every open file, its format, run selection, filters and highlighters; each view's column layout and wrap mode; presets, window geometry, the tab order and the pane layout — all come back. |
| 📐 **Multi-line records** | A record whose message spans lines occupies a taller row and is shown in full, in place. Oversized records cap at 100 lines and expand on request. |
| 🔎 **Find** | Regex or plain, case options, wrap-around — distinct from filtering; it moves the cursor and hides nothing. |
| 🌍 **Encodings & zones** | Auto-detected encoding (incl. the UTF-16 a `wchar_t` build writes on Windows), forceable. Timestamps are real points in time, with configurable source and display zones. |

## Quick start

```bash
loftail /var/log/myapp.log --pattern '%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n'
```

The file opens at its end and follows it. `--pattern` is only needed the first time — the format is remembered per file, and loftail will try to detect it anyway. There is deliberately no `--follow` flag: following is unconditional.

## Building

Requires **CMake ≥ 3.21**, **Ninja**, a **C++20** compiler, and **Qt 6.4+**. The reference environment is stock Ubuntu 24.04 (GCC 13, Qt 6.4.2 from the distro repos — no separately-installed Qt).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/src/loftail

ctest --test-dir build --output-on-failure    # 20 test binaries
```

On Windows and macOS, point `CMAKE_PREFIX_PATH` at your Qt installation. Packaging scripts for all three platforms (AppImage / portable zip / `.app` bundle, each with Qt bundled) live in [`packaging/`](packaging/README.md).

## How it works

A few decisions carry most of the weight:

- **The model stores byte offsets, never parsed text.** A record is `{offset, timestamp, length, loggerId, threadId, lineCount, priority}` — exactly 32 bytes. Message text is parsed lazily on the paint path.
- **A record is not a line.** A line matching the record-start regex begins a record; non-matching lines are continuations. Nothing assumes one row per line.
- **Filtering compares integers.** Logger and thread names are interned to `quint32` at index time; priority is an enum declared in severity order, so "minimum level" is one `>=`. Message-text matching has no integer fast path, so it runs last.
- **The table is a custom `QAbstractScrollArea`, not a `QTableView`,** scrolling in *line* units over two-level prefix sums of record line counts — because variable row heights can't be done lazily otherwise. It has an exact geometry mode and an estimated one for always-on wrapping.
- **The indexer is a single forward pass**, and file access goes through a `LogSource` interface — which is what keeps compressed and SSH-backed sources additive rather than a rewrite.
- **Nothing downstream of the parser knows about the pattern string.** Views, filters, and highlighters consume only a field map.

Measured on a 200 MB / 1.9M-record synthetic log (Release, warm file): **214 MB/s** indexing single-threaded, block-sum rebuild **0.45 ms per 1M records**, paint-frame cost comfortably inside the 60 fps budget.

Full rationale in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Status

Feature-complete against the first-release spec, including the post-1.0 autodetection milestone.

| | |
| --- | --- |
| **Linux** | Built, tested, and AppImage clean-run verified. |
| **Windows / macOS** | Build and packaging authored; runtime verification happens in CI. The macOS job is currently disabled. |

## Documentation

| File | Contents |
| --- | --- |
| [`SPEC.md`](SPEC.md) | User-visible behavior of the first release. The product definition. |
| [`FUTURE.md`](FUTURE.md) | Features planned for later releases, and the accommodations that keep them cheap. |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Internal technical decisions and their rationale. |
| [`PLAN.md`](PLAN.md) | Milestone-by-milestone implementation plan. |

## Non-goals

loftail is strictly a reader: it never edits, writes, or deletes log files. It does not support other logging frameworks, does not merge several files into one time-ordered view, does not do charts or alerting, and does not claim the system default handler for `.log` (it only advertises that it *can* open them).
