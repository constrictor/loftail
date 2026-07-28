# loftail — Planned for Later Releases

**Status:** Draft, 2026-07-20.
**Scope:** User-visible features not yet built. `SPEC.md` describes what has shipped; genuine never-goals live in its §11. This file is the product roadmap — a list of intended features, not a schedule or a commitment to any order.

Each item notes what already-shipped code does to accommodate it, so that adding the feature later is additive rather than a rewrite. Those accommodations are obligations on the code as it stands, not future work — they are the reason these features stay cheap to add. Shipped items are struck through and kept, so the record of which accommodation paid off is not lost.

---

## ~~Format autodetection~~ — shipped

Delivered in M8 and specified in `SPEC.md` §4: an unrecognized file's `ConversionPattern` is guessed and pre-filled into the Log Format dialog for confirmation, never applied silently, with manual entry remaining authoritative. The accommodation did its job — detection reaches the format layer through the existing `IFormatProvider` seam and produces a *pattern string* fed to the same `PatternCompiler` as manual entry, so it needed no new parsing path and no new UI. See `ARCHITECTURE.md` §9 for the three-layer design (candidate scoring → structural inference → fall back to manual).

## ~~Multiple open files~~ — shipped

Delivered in M9 and specified in `SPEC.md` §5a: several logs open at once as tabs in a central document area, with a second view onto one file available. The four accommodations this file used to list (a `Document` owning all per-file state, no "current file" global, panes binding by signal, a `documents` array in the settings schema) did their job — the work was additive, as intended. See `ARCHITECTURE.md` §12 for the implemented design.

Logs first shipped as dock widgets, which made them draggable into splits and floating windows; that was withdrawn because it put panes and logs in one shared arrangement, where an ordinary pane drag could land the Filters pane on top of the log being read. Side-by-side logs are worth having back — see below.

## Two logs side by side

Compare two logs, or two points in one log, without alt-tabbing: split the document area so two tabs are visible at once, vertically or horizontally.

**Already accommodated:** per-view state is entirely inside `DocumentView`/`LogView` (scroll, selection, wrap, columns, follow), several views onto one file already work, and the session stores views as an ordered array — so this is a layout change, not a state change. It belongs *inside* the document area (a splitter of tab groups, the shape every IDE uses), never by returning logs to the dock layout: what makes the current arrangement predictable is that the panes cannot reach into the document area, and that must survive. The session schema would gain a description of the split, which is why the `views` array is ordered rather than keyed by a layout blob.

## Compressed logs

Open rotated logs directly in their compressed form (`.gz` to start), without decompressing them by hand first.

**Already accommodated:** file access goes through the `LogSource` interface, and the indexer is constrained to a single forward pass with no seek-and-re-read — because gzip has no random access without an index. See `ARCHITECTURE.md` §6.2. A `CompressedLogSource` decompresses forward into a local cache that the paint path reads from.

## Remote log sources (SSH)

Retrieve and follow logs from remote hosts over SSH, rather than only local files. Live updates work the same way — the remote file is polled or streamed for appends.

**Already accommodated:** the same `LogSource` interface and single-forward-pass indexer as compressed logs; `isRandomAccess()` already lets the indexer branch for a source where every read carries latency. See `ARCHITECTURE.md` §6.2.

## Bookmarks

Mark records of interest and jump between them, so a spot found once can be returned to without re-searching. Bookmarks would be per file and part of the remembered session, and would add a pane alongside filters, highlighters, and presets.

**Not yet accommodated in detail**, but low-risk: a bookmark is a record identity plus a note, and the pane follows the same active-document-binding pattern as the others (`ARCHITECTURE.md` §12.3). The one design point to settle when it is built is what identifies a bookmarked record across a reindex or rotation — a byte offset is not stable, so it likely keys on timestamp plus a content hash.

---

## Relationship to other documents

- `SPEC.md` — what has shipped. When a feature here ships, its user-visible behavior moves into `SPEC.md` and its entry here is struck through, as "Multiple open files" now is.
- `ARCHITECTURE.md` — the accommodations referenced above are implemented, not deferred.
- `PLAN.md` — milestone M8 implemented format autodetection and M9 multiple open files (including the move from dock widgets to a document area). The remaining items here have no scheduled milestone yet and are listed in that file's "Deliberately deferred" section.
