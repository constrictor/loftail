# loftail — Planned for Later Releases

**Status:** Draft, 2026-07-20.
**Scope:** User-visible features intended for releases *after* the first. `SPEC.md` describes the first release only; genuine never-goals live in its §11. This file is the product roadmap beyond 1.0 — a list of intended features, not a schedule or a commitment to any order.

Each item notes what the first release already does to accommodate it, so that adding the feature later is additive rather than a rewrite. Those accommodations are P1 obligations, not future work — they are the reason these features stay cheap to add.

---

## Format autodetection

When a file is opened, loftail guesses its `ConversionPattern` and pre-fills the Log Format dialog with the guess, shown for confirmation rather than applied silently. Manual entry remains available and authoritative; the dialog is the same one used in the first release.

**Already accommodated:** the format layer is reached only through the `IFormatProvider` seam, and autodetection produces a *pattern string* fed to the same `PatternCompiler` as manual entry — so it needs no new parsing path and no new UI. See `ARCHITECTURE.md` §9 for the three-layer detection design (candidate scoring → structural inference → fall back to manual).

## Multiple open files

Several logs open at once, as tabs or a split view, each with its own format, filters, highlighters, and column layout. Per-file working state is already scoped and persisted per file (`SPEC.md` §10), so each file keeps how you were reading it.

**Already accommodated:** all per-file state lives in a `Document` type; nothing reaches for "the current file" globally; panes bind to the active document by signal; and the settings schema stores a `documents` array even while it holds one element today. See `ARCHITECTURE.md` §12. The remaining work is genuinely additive: a tab bar or split view, and per-document indexing threads.

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

- `SPEC.md` — the first release. If a feature here ever ships, its user-visible behavior moves into `SPEC.md`.
- `ARCHITECTURE.md` — the accommodations referenced above are implemented as part of the first release, not deferred.
- `PLAN.md` — milestone M8 implements format autodetection. The other items here have no scheduled milestone yet and are listed in that file's "Deliberately deferred" section.
