# loftail — Planned for Later Releases

**Status:** Draft, 2026-07-20.
**Scope:** User-visible features not yet built. `SPEC.md` describes what has shipped; genuine never-goals live in its §11. This file is the product roadmap — a list of intended features, not a schedule or a commitment to any order.

Each item notes what already-shipped code does to accommodate it, so that adding the feature later is additive rather than a rewrite. Those accommodations are obligations on the code as it stands, not future work — they are the reason these features stay cheap to add. Shipped items are struck through and kept, so the record of which accommodation paid off is not lost — including where it did not, which has been the more useful record so far.

---

## ~~Format autodetection~~ — shipped

Delivered in M8 and specified in `SPEC.md` §4: an unrecognized file's `ConversionPattern` is guessed and pre-filled into the Log Format dialog for confirmation, never applied silently, with manual entry remaining authoritative. The accommodation did its job — detection reaches the format layer through the existing `IFormatProvider` seam and produces a *pattern string* fed to the same `PatternCompiler` as manual entry, so it needed no new parsing path and no new UI. See `ARCHITECTURE.md` §9 for the three-layer design (candidate scoring → structural inference → fall back to manual).

## ~~Multiple open files~~ — shipped

Delivered in M9 and specified in `SPEC.md` §5a: several logs open at once as tabs in a central document area, with a second view onto one file available. The four accommodations this file used to list (a `Document` owning all per-file state, no "current file" global, panes binding by signal, a `documents` array in the settings schema) did their job — the work was additive, as intended. See `ARCHITECTURE.md` §12 for the implemented design.

Logs first shipped as dock widgets, which made them draggable into splits and floating windows; that was withdrawn because it put panes and logs in one shared arrangement, where an ordinary pane drag could land the Filters pane on top of the log being read. Side-by-side logs are worth having back — see below.

## Two logs side by side

Compare two logs, or two points in one log, without alt-tabbing: split the document area so two tabs are visible at once, vertically or horizontally.

**Already accommodated:** per-view state is entirely inside `DocumentView`/`LogView` (scroll, selection, wrap, columns, follow), several views onto one file already work, and the session stores views as an ordered array — so this is a layout change, not a state change. It belongs *inside* the document area (a splitter of tab groups, the shape every IDE uses), never by returning logs to the dock layout: what makes the current arrangement predictable is that the panes cannot reach into the document area, and that must survive. The session schema would gain a description of the split, which is why the `views` array is ordered rather than keyed by a layout blob.

## ~~Compressed logs~~ — shipped

~~Open rotated logs directly in their compressed form (`.gz` to start), without decompressing them by hand first.~~

**Shipped in M12**, and rather wider than `.gz`: zip, tar and every compressed tar too. The behavior is in `SPEC.md` §3 and the design in `ARCHITECTURE.md` §6.4.

**This entry's prediction was right, and it is the only one in this file that was.** "A local cache that the paint path reads from" is exactly what shipped, and because M11 had already built that cache the work really was *a second fetcher rather than a second mechanism* — `ArchiveFetcher` beside `SshFetcher`, behind the same spool. The single-forward-pass constraint paid for the second time, and more visibly than the first: an archived log fills in **as it expands** rather than freezing until it is done.

Two things the entry did not anticipate, both worth recording:

- **A compressed source needs a way to say its stream is finished**, which no other source needs and which nothing in `LogSource` could express. It arrived as a non-pure `isComplete()`, by exactly the route `wasReplaced()` took in M11 — the same pattern twice is now evidence that the interface's non-pure-virtual seam is the right shape for "only some sources can answer this".
- **An archive composes with a transport.** A log can be compressed *and* on another machine, and the two are orthogonal — a file type and a way of reaching a file. That fell out for free once the archive fetcher's input was itself an ordinary `LogSource`, but only because the address was spelled as a nested path rather than as a scheme of its own; `archive://` would have needed `archive+ssh://` next.

## Key files and key passphrases

`HostBookmark::keyFile` and `HostBookmark::Auth::KeyFile` have existed since M11 and are **dead**: nothing reads `keyFile` at connect time, and the Open Remote dialog's auth combo does not even offer the option. `SshSession::tryDefaultKeys()` walks `id_ed25519`, `id_ecdsa` and `id_rsa` with a deliberately **empty** passphrase, on the reasoning that a passphrase-protected key belongs in the agent and prompting for one would be a second, differently-shaped password dialog. So today a user whose key is not in an agent and does have a passphrase cannot use it at all, and has to fall back to password auth.

The accommodation that makes this cheap is already in place. M14's `SecretStore` is exactly what a passphrase needs — a place to keep a secret that is not the plain-text bookmark file — and `SshPrompter` already owns "ask a person for a secret, and decide where the answer is kept". The work is a named-key path through `authenticate()`, a second `askPassword()` shape whose prompt says *passphrase* and whose key in the store is the key file rather than the host, and the combo entry that has been missing since M11. Nothing about the seam changes.

Worth doing when someone actually hits it; recorded here because two persisted fields that no code reads look like an oversight rather than a decision, and because the M14 seam is the reason this is now additive.

## ~~Remote log sources (SSH)~~ — shipped

~~Retrieve and follow logs from remote hosts over SSH, rather than only local files. Live updates work the same way — the remote file is polled or streamed for appends.~~

**Shipped in M11**; the behavior is in `SPEC.md` §3 and the design in `ARCHITECTURE.md` §6.3. The accommodation that paid off was *not* the one this entry named. `isRandomAccess()` was expected to become false for a remote source, and it did not: fetching forward into a local spool file makes a remote log randomly seekable after all, so the paint path needed no change whatsoever. What actually made this additive was the **single-forward-pass indexer** — because the indexer only ever scans forward and never seeks back, the spool can be filled and indexed at the same time, which is what turns a remote log into a live one rather than a download. The `LogSource` interface itself held up exactly as intended: two of its six methods gained a remote implementation and nothing above it changed.

## ~~Logs that are not there~~ — shipped

**Shipped in M13**, and it was never in this file, which is the point worth recording. Opening a log before it exists, and keeping one that is deleted while open, were not on the roadmap at all — they were asked for as a question about what loftail *did* when a file was missing, and the honest answer was "refuses, with a message". The behaviour is in `SPEC.md` §3 and the design in `ARCHITECTURE.md` §6.5.

No accommodation was named in advance, and yet it was additive anyway, for a reason worth carrying forward: the **non-pure-virtual seam on `LogSource`** absorbed it, exactly as it had absorbed `wasReplaced()` in M11 and `isComplete()` in M12. Three features, three questions only some sources can answer, one shape. That is now enough of a pattern to be treated as the intended way to extend the interface rather than a coincidence — and it is the accommodation to protect, more than any particular flag.

The prediction this file would have got wrong, had it made one, is where the work landed. It looks like a `LogSource` feature ("a source for a file that is not there") and is not one: what changes is not how bytes are read but whether there are any yet, which is the live controller's existing question. Waiting is a state of the live seam, and no new source type exists.

## Bookmarks

Mark records of interest and jump between them, so a spot found once can be returned to without re-searching. Bookmarks would be per file and part of the remembered session, and would add a pane alongside filters, highlighters, and presets.

**Not yet accommodated in detail**, but low-risk: a bookmark is a record identity plus a note, and the pane follows the same active-document-binding pattern as the others (`ARCHITECTURE.md` §12.3). The one design point to settle when it is built is what identifies a bookmarked record across a reindex or rotation — a byte offset is not stable, so it likely keys on timestamp plus a content hash.

---

## Relationship to other documents

- `SPEC.md` — what has shipped. When a feature here ships, its user-visible behavior moves into `SPEC.md` and its entry here is struck through, as "Multiple open files" now is.
- `ARCHITECTURE.md` — the accommodations referenced above are implemented, not deferred.
- `PLAN.md` — milestone M8 implemented format autodetection, M9 multiple open files (including the move from dock widgets to a document area), M11 remote logs over SSH, M12 compressed and archived logs, and M13 logs that are not there. The remaining items here have no scheduled milestone yet and are listed in that file's "Deliberately deferred" section.
