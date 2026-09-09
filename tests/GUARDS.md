# GUARDS.md — the rules, and what pins them

CLAUDE.md names on the order of six hundred load-bearing decisions, most of them
in paragraphs of the form "Six M17 rules that are easy to undo by accident", and
it usually names the test case that pins one: "`tst_welcome::returnOnASelectedRowOpensItToo`
is the only case to catch it". Nothing checked that mapping. A case can be
renamed, deleted or quietly weakened and the prose goes stale with no signal at
all — which is not hypothetical here: CLAUDE.md itself records
`everyColumnStartsWideEnoughForItsOwnHeading` (which did neither of the two
things needed to see the bug), `severalPickedMembersOpenAsSeveralTabs` (which
asserted the tab count and nothing else) and
`tst_remotelocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath` (whose table
had no remote row) as cases that guarded nothing while appearing to.

This file is that mapping, written down so it can be checked. `tests/check_guards.py`
runs as the `guards_index` CTest case and asserts, for every guard named below,
that the binary exists in this configuration and reports that case. A binary the
current configuration did not build (SSH, archive, keychain, presets; the two
POSIX-only tail tests) is skipped, not failed. A name with no `tests/tst_<x>.cpp`
behind it fails as a typo.

Every guard here is one CLAUDE.md actually names in connection with that rule.
Nothing was inferred from the tests directory: an unguarded rule is a finding,
not a gap to paper over, which is what the second table is. bugs.md names no
test cases at all, so CLAUDE.md is the whole source.

Two shapes of entry:

* `tst_binary::caseName` — the prose names a case.
* `tst_binary` — the prose names only a binary ("`tst_socketdetach` pins it",
  "pinned by the ungated `tst_sshretry`"). Checked for existence only.

The **CLAUDE.md** column is a short handle for the paragraph plus the line the
paragraph starts on, as of the commit that added this file. Line numbers drift;
the handle is what to search for.

The mutation harness in `tests/mutations/` is the other half of this: an index
says a guard exists, and a mutation says it bites. See
`tests/mutations/run-mutations.sh`.

`tests/shuffle_cases.py` is a third half, and it guards something no entry below
can: that a case passes for its own reasons rather than because of what ran
before it. It permutes the cases of each binary inside one process — which
`ctest --schedule-random` cannot do, that flag reordering the binaries while the
state that leaks is per-process — and the nightly `flake-hunt` workflow runs it
beside `--repeat until-fail`. Four suites failed the first shuffled run; see
CLAUDE.md, "THE SUITE HAD NEVER BEEN RUN IN ANY ORDER BUT ITS OWN".
`tst_sessiongui` is excluded there by name, its cases chaining through the
session on purpose.

A COST contract is guarded the same way and never with a wall clock: the guard
counts the operations the contract is about — glyph measurements per codepoint,
records re-measured per ingest tick and per frame, rows scanned per slice, rows
asked per Find tally — because a count is exact, is reproducible on a shared
runner, and fails by an order of magnitude when the contract goes. The one
wall-clock check in the tree is `bench_index --selftest`, which is labelled
`perf`, is DISABLED unless `-DLOFTAIL_PERF_TESTS=ON`, and gates nothing.

## Guarded — 192 rules

| Rule | CLAUDE.md | Guard |
| --- | --- | --- |
| Activation is `itemActivated`, never `itemDoubleClicked`, or the list is unreachable from a keyboard | Welcome screen (L11) | tst_welcome::returnOnASelectedRowOpensItToo |
| The content column is centred by a stretch either side and carries a stretch of its own | Welcome layout (L11) | tst_welcome::theContentIsCentredRatherThanFillingTheWindow |
| A stretch at each end of the column, so the slack is a fifth of the viewport rather than merely non-zero | Welcome layout (L11) | tst_welcome::theContentIsCentredRatherThanFillingTheWindow |
| That case derives its window height from the content it just measured rather than writing 900 down | Welcome layout (L11) | tst_welcome::theContentIsCentredRatherThanFillingTheWindow |
| The name wears the application mark, DRAWN AND NEVER LOADED; `AppIcon.cpp` and the SVG are kept in step by hand | Welcome mark (L11) | tst_welcome::theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether |
| The mark is a widget that paints, not a `QLabel` holding a pixmap | Welcome mark (L11) | tst_welcome::theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether |
| The arrow's two subpaths are one path with `Qt::WindingFill`, or the overlap is punched back out | Welcome mark (L11) | tst_welcome::theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether |
| The mark's colours are fixed and never palette-derived, this being a logo rather than chrome | Welcome mark (L11) | tst_welcome::theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether |
| The mark and the name are centred as a PAIR, the size taken from the title's own metrics | Welcome mark (L11) | tst_welcome::theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether |
| The zebra is supplied by `UiColors::alternateRowColor()`, never left to `QPalette::AlternateBase`, and re-taken on `PaletteChange` | Welcome zebra (L11) | tst_welcome::aThemeThatSuppliesNoBandStillGetsOne |
| A launch that NAMES files does not restore the session's tabs (`SessionRestore::ShellOnly`) | Named-file launch (L15) | tst_tabsession::namedFilesReplaceTheSessionsTabsAndKeepItsShell |
| The geometry and the pane layout come back in BOTH modes | Named-file launch (L15) | tst_tabsession::namedFilesReplaceTheSessionsTabsAndKeepItsShell |
| The early return is after the `restoreState()` block and before `beginBulkRestore()`, and calls `updateEmptyState()` | Named-file launch (L15) | tst_tabsession::namedFilesReplaceTheSessionsTabsAndKeepItsShell |
| The decision is the constructor's parameter, not a close-everything pass in `main()` | Named-file launch (L15) | tst_tabsession::namedFilesReplaceTheSessionsTabsAndKeepItsShell |
| The old tab set is deliberately not preserved anywhere | Named-file launch (L15) | tst_tabsession::namedFilesReplaceTheSessionsTabsAndKeepItsShell |
| Every address has a non-empty display name, falling back to the deepest segment, then the scheme word, then `(unnamed)` | Display name (L19) | tst_remotelocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath |
| The fallback must stay a SEGMENT and never a path, or `prefixedLabelsFor()` stops grouping | Display name (L19) | tst_remotelocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath |
| `logSourceBareName()` is the name with the bracket off and is `tabLabelsFor()`'s grouping key, so it may not hold a path | Display name (L19) | tst_remotelocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath |
| `RemoteLocation::withoutPassword()` is the one filter the name half, `logSourceDisplayPath()` and `LogSourceFactory` all ask | Display name (L19) | tst_hostbookmarks::aPasswordNeverLeaksIntoAPathString<br>tst_remotelocation::anAddressThatDoesNotParseStillLosesItsPassword |
| `RemoteLocation::normalize()` is IDEMPOTENT, which is what every entry point normalizing and `Document::prepare()` normalizing again rests on | One log, one spelling (L223) | tst_remotelocation::normalizingAnAddressTwiceIsNormalizingItOnce |
| An address holding a Unicode noncharacter is REFUSED at `parse()` — all three components — rather than respelled into a second spelling | One log, one spelling (L223) | tst_remotelocation::anAddressHoldingANoncharacterIsRefusedRatherThanRespelled |
| `normalizeLogPath()` and `logSettingsKey()` are IDEMPOTENT, `QDir::cleanPath()` not being idempotent itself | One log, one spelling (L223) | tst_archivelocation::theSecondNormalizeMovesNothingHoweverTheContainerIsSpelled |
| `cleanedToFixedPoint()` is a LOOP with a bound, not a second call, and falls out with the last value | One log, one spelling (L223) | tst_archivelocation::theSecondNormalizeMovesNothingHoweverTheContainerIsSpelled |
| It never makes a path LESS absolute, a Qt resource path coming back from `absoluteFilePath()` unchanged | One log, one spelling (L223) | tst_archivelocation::theSecondNormalizeMovesNothingHoweverTheContainerIsSpelled |
| A port outside 1..65535 is a malformed address refused at `parse()`, not a port carried into a connect and reported as a refusal by the far end | Display name (L19) | tst_remotelocation::aPortOutsideTheTcpRangeIsARefusedAddressAndNotAFailedConnect |
| An address holding a NUL is REFUSED at `logPathIsWellFormed()` — before the archive split, so the plain, remote, container and member routes are one comparison | One log, one spelling (L223) | tst_remotelocation::anAddressHoldingANulIsRefusedRatherThanRekeyed |
| `absoluteLocalPath()` therefore hands such a path back untouched, `QFileInfo` answering a broken filename with one that is still relative | One log, one spelling (L223) | tst_remotelocation::anAddressHoldingANulIsRefusedRatherThanRekeyed<br>tst_archivelocation::theSecondNormalizeMovesNothingHoweverTheContainerIsSpelled |
| A remote archive member is OPAQUE — taken off the RAW address and appended verbatim — while the container half goes on being normalized as a URL | One log, one spelling (L223) | tst_archivelocation::theContainerIsNormalizedAsAUrlAndTheMemberIsNot |
| The cut is taken in the raw path the member comes out of, never transplanted from the decoded one, whose lengths are a different arithmetic | One log, one spelling (L223) | tst_archivelocation::theSecondNormalizeMovesNothingHoweverTheContainerIsSpelled |
| A name run gives up its optional trailing dot exactly where the format spells a literal one, in the regex and in `readWord()` together | Date format (L21) | tst_timestampparser::aFullStopTheFormatSpellsBelongsToTheFormatAndNotToTheNameBeforeIt |
| The regression test must reject any modal dialog that is not the picker, or it hangs rather than failing | Nested member (L33) | tst_archiveopen |
| Every multi-member case counts records PER TAB via `recordsInTab()`, not just the tab count | Nested member (L33) | tst_archiveopen::severalPickedMembersOpenAsSeveralTabs |
| The archive fixtures are built at runtime by libarchive's own write side; nothing binary is committed | libarchive in CI (L37) | tst_archivefetcher<br>tst_archivetail<br>tst_archivemembers<br>tst_archiveopen |
| The path layer and the completion contract are ungated and run in every configuration | libarchive in CI (L37) | tst_archivelocation<br>tst_complete |
| The SSH socket must not stay a `QTcpSocket`; `SocketDetach.h` dups the descriptor and lets Qt close its own | SSH socket (L39) | tst_socketdetach |
| A connect is sliced with a `QEventLoop`, never with `waitForConnected()`, whose timeout resets the socket layer | Connect slices (L41) | tst_socketdetach::aTimedOutWaitForConnectedAbandonsTheAttempt<br>tst_socketdetach::aConnectSlowerThanOneSliceStillConnects |
| `SshExecCommands` and `ExecSizeProbe` are always compiled, `shellQuote()` being a security boundary | SFTP fallback (L41) | tst_sshexec<br>tst_execsizeprobe |
| Three containerised sshd servers, because a stock sshd reaches neither the exec transport nor the lower size rungs | SSH CI (L47) | tst_sshlive |
| Everything above `RemoteFetcher` is covered with no network at all over `tests/FakeFetcher.h` | SSH CI (L47) | tst_spooledsource<br>tst_remotetail<br>tst_remoteopen |
| (a) `readAt()` seeks only on a discontinuity, a seek flushing libssh2's read-ahead | SSH slow (L49) | tst_sshlive |
| (a) `Impl::filePos` is written ONLY by `adoptFile()`, or one generation's bytes splice onto another's spool | SSH slow (L49) | tst_sshlive |
| (a) `filePos` is advanced by bytes actually read, short reads included, and reset on the error branch | SSH slow (L49) | tst_sshlive |
| (b) `Need::ExecOnly` stops a connect after the login rather than always calling `libssh2_sftp_init()` | SSH slow (L49) | tst_sshlive |
| (b) Such a session is `Mode::Exec` and not a third enumerator, with `Impl::execOnly` making six calls refuse BY NAME | SSH slow (L49) | tst_sshlive |
| (c) `SshSessionCache` is 60 s idle, capped at 4, keyed on target plus role, checked out on use | SSH slow (L49) | tst_sshsessioncache |
| (c) An `ExecOnly` errand may borrow a `LogTransport` session and never the reverse | SSH slow (L49) | tst_sshsessioncache |
| (d) An exec-mode catch-up is ONE `tail`: `streamReadCommand()` is `readCommand()` with `head -c L` dropped | SSH slow (L49) | tst_sshexec::theStreamingReadIsNotBoundedByAnyLength |
| (f) Compression is per host, OFF, under Advanced, and an ARGUMENT on `connectTo()` rather than a setter | SSH slow (L49) | tst_sshoptions<br>tst_hostbookmarks<br>tst_remotedialog |
| `tests/FakeFetcher.h` cannot prove the non-blocking contract; a transport that genuinely takes a second is what pins it | FakeFetcher (L61) | tst_asyncconnect |
| Both non-blocking budgets are a deliberately loose second (`kNonBlockingMs`) | FakeFetcher (L61) | tst_asyncconnect |
| `LogProfile::operator==` must gain a clause for every field, or `reduce()` makes it silent data loss | Six M22 rules (L67) | tst_logsettings::aProfileDiffersWhenAnyOneFieldOfItDoes |
| Every path a shell sees goes through `shellQuote()` | Remote config (L71) | tst_sshexec |
| `ConfigFileIO` never creates a directory, refused by name | Config write (L69) | tst_configeditor::aMissingDirectoryIsRefusedByName<br>tst_writefailure::aConfigInADirectoryThatIsNotThereIsRefusedByNameAndNothingIsCreated |
| It replays the encoding, BOM and line endings it read, through `Decoder::encode()` | Config write (L69) | tst_configeditor::aSavedConfigKeepsTheEncodingTheBomAndTheLineEndingsItWasReadWith |
| `decode()` and `encode()` are INVERSE: `ConvertInitialBom` in every branch, so a mark past `bomLength()` is a character of the text | Config write (L69) | tst_decoder::aZeroWidthNoBreakSpaceInTheTextSurvivesBothDirections<br>tst_decoder::aMarkAfterTheFilesOwnMarkIsACharacterOfTheText<br>tst_configeditor::aConfigWhoseTextBeginsWithAMarkIsSavedWhole |
| A save whose bytes would not read back as what is on screen is REFUSED, with nothing written and the buffer keeping its edits | Config write (L69) | tst_configeditor::aSaveThatWouldNotReadBackIsRefusedAndTheEditsStay<br>tst_writefailure::bytesThatWouldNotReadBackAsWhatIsOnScreenAreRefusedAndNamed |
| `ConfigView::bytesToSave()` is the one way in, so no save site can walk round the guard | Config write (L69) | tst_configeditor::aSaveThatWouldNotReadBackIsRefusedAndTheEditsStay |
| A config write that cannot be finished is REPORTED, and the file keeps what it had | Config write (L69) | tst_writefailure::aConfigWriteThatCannotBeFinishedIsReportedAndKeepsThePreviousContents |
| "Not there" and "there and shut" are different sentences, and only the first is the supported empty-editor case | Presence not emptiness (L207) | tst_writefailure::aConfigThatIsThereAndShutIsNotDescribedAsOneThatIsNotThere |
| The libssh2 half of the config write now runs on every push against real servers | Config write in CI (L73) | tst_sshlive::aConfigFileIsReadAndWrittenWholeOverSftp<br>tst_sshlive::writingAConfigKeepsItsPermissions<br>tst_sshlive::theExecFallbackWritesTheSameBytes |
| `logAnchorOf()` and `SshWorkerPool` were EXTRACTED rather than copied, the untouched suites being the evidence | M23 (L75) | tst_configlocation<br>tst_configeditor |
| The values are quoted and the SCRIPT IS NOT; getting it backwards fails silently in both directions | Nine M23 rules (L77) | tst_sshexec |
| A local run's output goes to FILES, never pipes, or closing the read end SIGPIPEs the daemon | Nine M23 rules (L77) | tst_restartrunner::closingTheRunDoesNotKillWhatTheScriptStarted |
| `LogProfileEditor::profile()` trims the ENDS, or a stray newline gives every log an entry of its own | Nine M23 rules (L77) | tst_preferences::trailingWhitespaceInTheRestartScriptLeavesNoEntryBehind |
| The libssh2 half of M23 now runs on every push | M23 in CI (L81) | tst_sshlive::aRestartScriptRunsOnTheFarEndAndKeepsItsStderr<br>tst_sshlive::aRestartScriptOutlivesTheConnectTimeout<br>tst_sshlive::abortingARemoteScriptReturnsAtOnce<br>tst_sshlive::aRestartScriptRunsOnAnExecOnlyConnect |
| The restart command text is proven with no server through a real `/bin/sh` | M23 in CI (L81) | tst_sshexec |
| The whole local restart branch is proven against real scripts | M23 in CI (L81) | tst_restartrunner |
| The restart address derivation is proven with no server | M23 in CI (L81) | tst_restarttarget |
| A mark is placed in LINE units through `LogView::scrollFractionOfRow()`, never by record index | Six density rules (L95) | tst_densitybar::aMarkSitsWhereTheScrollbarWouldPutItsRecord |
| The `densityStrip` `QSettings` key must be removed per case, or the suite passes on test order | Six density rules (L95) | tst_densitybar::init |
| The arrow buttons' width cap is a `setMaximumWidth` and nothing else, with no `QSizePolicy::Ignored` | Copy button row (L103) | tst_highlighterpane::theCopyButtonRidesTheRowTheArrowsPaidFor |
| The height is capped too, to the worded neighbour's own hint | Copy button row (L103) | tst_highlighterpane::theCopyButtonRidesTheRowTheArrowsPaidFor |
| Both numbers are measured from the neighbouring button and from the style, never written down | Copy button row (L103) | tst_highlighterpane::theCopyButtonRidesTheRowTheArrowsPaidFor |
| The glyphs are not translated, the words going on `setToolTip` and `setAccessibleName`, so `ruleUp`/`ruleDown` are the only handle | Copy button row (L103) | tst_highlighterpane::theCopyButtonRidesTheRowTheArrowsPaidFor |
| That case states RELATIONS, never a pixel count, Fusion and Breeze disagreeing about every term | Copy button row (L103) | tst_highlighterpane::theCopyButtonRidesTheRowTheArrowsPaidFor |
| An EMPTY wait reason must never be published, or a local wait's sentence is blanked on the first tick | Waiting reason republished (L109) | tst_waitingremote::aLocalWaitKeepsTheReasonItWasGiven |
| A fake refusal published synchronously from `start()` is a different shape from one that arrives on a later tick | Waiting reason republished (L109) | tst_remoteopen::aTransportRefusalKeepsTheTabAndSaysWhy |
| `ReconnectGrace` is gated on having SIGNED IN ONCE — a tab that never connected has no evidence its credentials work | Machine that reboots (L111) | tst_sshretry |
| The grace is BOUNDED (five minutes) and then latches exactly as before | Machine that reboots (L111) | tst_sshretry |
| `signedIn()` CLEARS the grace window rather than being ignored inside it, so a second reboot is survived too | Machine that reboots (L111) | tst_sshretry |
| The retry is unattended by construction — `m_wantsPrompter` is consumed at the top of `reconnect()` | Machine that reboots (L111) | tst_sshretry |
| `LiveController::start()` asks BEFORE `syncBaseline()` and through `refreshSize()`, never `size()` | Format judged on real bytes (L117) | tst_archiveopen::theFormatAndTheSessionRememberAnArchivedPath |
| `ReloadCause` is derived from `wasReplaced()` and a shrink, and NEVER read off the catch-all `wasTruncated()` | Rotation announced (L119) | tst_tail::overwriteInPlaceTriggersRescan |
| The new signal sits BESIDE the nullary `rescanned()` rather than differentiating it | Rotation announced (L119) | tst_tail |
| `HostBookmarkStore::find()` is called at connect time, so a bookmark's saved password is read back | M14 dangling wires (L129) | tst_sshcredentials |
| `available()` is a round trip and not a link check — a real backend must answer | Real Secret Service in CI (L131) | tst_keychainlive::roundTripsASecret |
| Storing the same key twice REPLACES rather than adding a second item under it | Real Secret Service in CI (L131) | tst_keychainlive::storingTwiceReplacesTheSecretRatherThanAddingOne |
| A secret survives being long and not ASCII across the backend | Real Secret Service in CI (L131) | tst_keychainlive::aSecretSurvivesBeingLongAndNotAscii |
| A keychain is consulted only on the thread that opened the log, and `available()` does not LATCH off it | Real Secret Service in CI (L131) | tst_keychainlive::everyOperationRefusesToRunOffTheApplicationThread |
| The thread guard sits ABOVE the probe latch, so a store that has probed refuses off-thread as well | Three M14 rules (L127) | tst_keychainlive::everyOperationRefusesToRunOffTheApplicationThread |
| Every route reaches a store through `secretStore()`, which marshals, so no caller can see that refusal | Three M14 rules (L127) | tst_secretstore::everyCallReachesTheStoreOnTheApplicationThread |
| Erasing what is not there succeeds, `forgetSshPassword()` running on every rejected stored password | Real Secret Service in CI (L131) | tst_keychainlive::erasingWhatIsNotThereSucceeds |
| The context spinners are capped with `setMaximumWidth` and nothing else | Two load-bearing widths (L143) | tst_filterpane::theContextRowLaysOutWithoutOverlapOrClipping |
| `QSizePolicy::Ignored` must NOT be added to the context spinners — the combination lays the row on top of itself | Two load-bearing widths (L143) | tst_filterpane::theContextRowLaysOutWithoutOverlapOrClipping |
| The axis order is Priority, Subsystem, Message text, Thread, Time range | Three later pane changes (L147) | tst_filterpane::theAxesAreInReadingOrder |
| The two value lists are the only growing things: stretch 1 each, and the trailing `root->addStretch(1)` is GONE | Three later pane changes (L147) | tst_filterpane::theValueListsTakeTheSpareHeight |
| Both value axes take the SAME stretch factor, so the split cannot shift as the scan finds more subsystems | Three later pane changes (L147) | tst_filterpane::theValueListsTakeTheSpareHeight |
| `kPriorityByIndex` must keep all six levels; the combo skips TRACE and `comboPriority()` is the only bridge | Three later pane changes (L147) | tst_filterpane::theLevelsOfferedStartAtDebugAndDefaultToInfo |
| A stored TRACE floor is applied as an UNTICKED axis, never promoted to DEBUG | Three later pane changes (L147) | tst_filterpane::aRestoredTraceFloorBecomesAnUntickedAxis |
| The Filters pane has no header row; `updateActivity()` keeps its change guard, the dock title being a `QTabBar` entry | Three later pane changes (L147) | tst_panechrome::theFiltersTabIsMarkedWhileFiltersAreInForce |
| A selection that narrows is armed as `setCriteria()` loads it; `criteria()` then reports the memo, not the widget | A selection loaded before the scan (L153) | tst_filterpane::aSelectionHydratedOverAnUnscannedLogStillNarrowsSomething |
| An armed axis rebuilds its rows under `ListRule::Load`, so a discovered name is ticked as the selection says | A selection loaded before the scan (L153) | tst_multidoc::aSubsystemSelectionSurvivesClosingAndReopeningTheLog |
| It settles on a listed value the memo does NOT name — never on the memo's own names being listed | A selection loaded before the scan (L153) | tst_multidoc::aSubsystemSelectionSurvivesClosingAndReopeningTheLog |
| An EMPTY log stays armed, so its record survives a launch with nothing on screen to enforce a selection over | A selection loaded before the scan (L153) | tst_multidoc::aSelectionSurvivesARestartOnWhichTheLogIsEmpty |
| `AxisEditor`'s `seen` is a `QHash<QString, bool>`, so a name off screen returns in the state it left in | Rotation empties the lists (L149) | tst_filterpane |
| The memo is read ONLY for a name that is not currently a row — on screen, the widget is the truth | Rotation empties the lists (L149) | tst_filterpane |
| The memo is written from the list's `itemChanged` handler, not at the next repopulation | Rotation empties the lists (L149) | tst_filterpane |
| That write is suppressed while `m_populating`, or `ListRule::Unstated`'s un-seeing is undone by its own rows | Rotation empties the lists (L149) | tst_filterpane |
| Every palette slot names the neutral that reads on it, clearing 4.5:1 in BOTH themes | Highlight palette bands (L155) | tst_highlight::paletteEveryBackgroundHasReadableText |
| The Vivid band stays out of the dead luminance band ~0.16–0.21, both variants of a hue on one side of it | Highlight palette bands (L155) | tst_highlight::paletteEveryBackgroundHasReadableText |
| `HighlighterPane::addRule()` sets the FOREGROUND as well as the background | Highlight palette bands (L155) | tst_highlight::paletteEveryBackgroundHasReadableText |
| The zebra band is `UiColors::alternateRowColor()` and NOT `QPalette::AlternateBase` | Zebra band contrast (L159) | tst_logview::theAlternatingBandChangesAtRecordsAndNotAtLines<br>tst_uicolors |
| The band is keyed on the view ROW, which is a record and not a line | Zebra band contrast (L159) | tst_logview::theAlternatingBandChangesAtRecordsAndNotAtLines |
| The band stays in the `else` of the highlight-rule branch, or two adjacent matches read as two rules | Zebra band contrast (L159) | tst_logview::theAlternatingBandChangesAtRecordsAndNotAtLines |
| No column is bounded below `seedFloorOf()`, its caption-and-typical-value width | Seed bounded by viewport (L165) | tst_multidoc::theScanCompletionSeedLeavesTheMessageColumnOnScreen |
| Tests derive their widths from the unbounded seed rather than writing a window size down | Seed bounded by viewport (L165) | tst_logview |
| `lineHeight()` is `qCeil(QFontMetricsF(fontMetrics()).height())`, never `QFontMetrics::height()` | Line pitch (L167) | tst_logview::everyWrappedLineOfARecordIsDrawnInsideTheRowItWasGiven |
| `measureWrappedLines()` divides a `boundingRect` height by `lineHeight()`, so the two must be one unit | Line pitch (L167) | tst_logview::aSelectedRecordIsGivenExactlyTheLinesItsWrappedTextTakes |
| The regression case must run at a point size where the two metrics actually DISAGREE | Line pitch (L167) | tst_logview::everyWrappedLineOfARecordIsDrawnInsideTheRowItWasGiven |
| The header caption inset comes from `headerLabelInset()` asking the style for `SE_HeaderLabel` | Header caption inset (L171) | tst_logview::everyColumnStartsWideEnoughForItsOwnHeading |
| The inset is measured over a SYNTHETIC rect of known width, the seed running before layout | Header caption inset (L171) | tst_logview::everyColumnStartsWideEnoughForItsOwnHeading |
| The inset is added to the caption term only, in `seedWidthOf()` AND `contentWidthOf()` | Header caption inset (L171) | tst_logview::everyColumnStartsWideEnoughForItsOwnHeading |
| A per-role allowance is measured as ONE string, `charsWidth(n)`, never `n * charWidth()` | Header caption inset (L171) | tst_logview::everyColumnStartsWideEnoughForItsOwnHeading |
| The caption case must measure the LABEL RECT and force `PM_HeaderMargin >= 6` through a `QProxyStyle` | Header caption inset (L171) | tst_logview::everyColumnStartsWideEnoughForItsOwnHeading |
| An axis inset case compares LEFT insets across panes and the right one per pane against that pane's own bar | Pair-pinning trap (L173) | tst_highlighterpane::theAxesSitWhereTheFiltersPanesDo |
| A geometry case asks for a VIEWPORT width and adds the frame itself, never a widget size plus a chrome assumption | Pair-pinning trap (L173) | tst_logview::theSeedKeepsTheMessageColumnOnScreenWhereTheNamesAreLong |
| `LogView::messageWrapWidth()` is the ONE expression every wrap width comes from; a fifth site restores the bug | One wrap width (L183) | tst_logview |
| The wrap floor stays in CHARACTERS and at or below ~25 columns, `aimWrapWidth()` aiming that low | One wrap width (L183) | tst_logview |
| The last-column branch of `messageWrapWidth()` is byte-for-byte what shipped | Message not last column (L185) | tst_logview |
| "Last" is walked back from the end of the VISUAL order past hidden sections, never `count() - 1` | Message not last column (L185) | tst_logview |
| The paint CLIPS at the section and does not lay the text out narrower | Message not last column (L185) | tst_logview |
| `wrappedMessageColumn()` is -1 for a HIDDEN message column as well as for a format with no `%m` | Message not last column (L185) | tst_logview |
| `columnIsOnScreen()` reads the section's WIDTH, not `isSectionHidden()`, which is set after the resize | Message not last column (L185) | tst_logview |
| `HighlighterPane::m_updating` is SAVED and RESTORED, never forced false on the way out | m_updating save/restore (L191) | tst_highlighterpane::reloadingTheListKeepsRulesEnabled |
| A rule the user turned off stays off across a rebuild | m_updating save/restore (L191) | tst_highlighterpane::reloadingTheListKeepsRulesEnabled |
| The M17 call-gate rule 2 needs a cancel raised from INSIDE the work through a nested event loop; a timer-posted cancel can never land while `drain()` runs | Sanitizers do not create coverage (L201) | tst_gatestress |
| The OK re-read is conditional and must stay so — a visit that moves nothing costs no rescan | Preferences OK re-reads (L211) | tst_openflow::okLeavesTheLogAloneWhenNothingAboutItMoved |
| `--pattern` overrides the resolved pattern and is then judged against the log like every other level | --pattern wins (L215) | tst_openflow (six `…SuppliedPattern…` cases) |
| `openWithSettings()`'s `promptIfNoMatch` parameter is gone; only `nothingToJudgeYet` may still suppress the prompt | --pattern wins (L215) | tst_openflow (six `…SuppliedPattern…` cases) |
| A background tab must not be arranged by passing a pattern, which silently switches the prompt off | --pattern wins (L215) | tst_remoteopen::aBackgroundResumeRaisesNoFormatDialog |
| `applyInitialSplit()` runs from `showEvent()` and must `layout()->activate()` first, or the split is computed against a zero-width splitter | Preferences splitter (L217) | tst_preferences |
| The split is guarded by `m_splitSettled`, which `splitterMoved` also sets, so a `rebuildTree()` never takes the handle back off the user | Preferences splitter (L217) | tst_preferences |
| The tree pane width is capped at 40% of the splitter, and every tree row carries a tooltip | Preferences splitter (L217) | tst_preferences |
| The splitter regression case must measure at TWO font sizes; the offscreen default alone passes with the bug in place | Preferences splitter (L217) | tst_preferences |
| `ctx->fileSettings` is held EXACTLY as it was read, or the first write looks like no change | Ten M21 rules (L221) | tst_openflow::aSuppliedPatternThatFitsIsRememberedForTheLog |
| A write goes slot file FIRST and the map second, so what an interruption leaves is always an unreferenced file | Ten M21 rules (L221) | tst_writefailure::aSaveWhoseSlotFileCannotBeWrittenNeverPutsTheAddressInTheMap<br>tst_writefailure::aSaveWhoseMapCannotBeWrittenLeavesTheRecordAsAnUnreferencedFile |
| Every record names its own address inside the slot file and `read()` checks it; map entries are an ARRAY so a duplicate is visible | Ten M21 rules (L221) | tst_logfilestore::aSlotHoldingAnotherLogsRecordIsNotServed<br>tst_writefailure::everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord |
| No interruption of a pool write serves one log another log's record; the states a crash can leave are enumerated, not sampled | Ten M21 rules (L221) | tst_writefailure::everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord |
| `AtomicJson::writePrivate()` restricts the mode AFTER the rename, the rename making a new inode every time | M14 keychain (L203) | tst_writefailure::aPrivateWriteLeavesTheSecretReadableByNobodyElse |
| A short write is never taken for the whole thing: `commit()` renames a truncated temporary and reports success | M21 one file per log (L219) | tst_writefailure::aWriteThatCannotBeFinishedIsReportedAndKeepsThePreviousContents |
| Absent means "nobody ever said anything" and seeds defaults; an EMPTY list is a deletion and must stay deleted — four stores deep | Presence, not emptiness (L225) | tst_sessiongui::aDeletedDefaultRuleStaysDeletedAcrossARelaunch |
| The `LogModel::setViewIndex()`/`viewGeometry()`/`sourceRow()` seam is inert: the defaults reproduce the old behaviour exactly | M19 is one seam (L233) | tst_logmodel<br>tst_logview<br>tst_filtercontext<br>tst_tail |
| `LogView::sizeHint()` must stay a pure query; `refreshDigestCap()` is the mutating half | Six M19 rules (L237) | tst_multidoc |
| The tray degrade path (control disabled with a reason; a Notify rule behaves as Tab) is the common path on the reference desktop | Notification has no CI cover (L241) | tst_highlighterpane |
| The notification rate limit is decided with an injected clock | Notification has no CI cover (L241) | tst_alertpolicy |
| `HighlightRule::operator==` and the operators under it compare EVERY field of their structs | Highlighters marker (L247) | tst_highlight::aRuleDiffersWhenAnyOneFieldOfItDoes |
| The two pickers are told apart by the SHAPE of the tile (edge-to-edge bar vs inset chip), not by colour | Swatch previews the pair (L249) | tst_highlighterpane::theTwoPickersAreTellableApartAtTheSizeTheyAreDrawn |
| The restore asks `d.highlighters.contains("rules")`, never the array's emptiness | Seeded FATAL/ERROR/WARN (L251) | tst_sessiongui::aDeletedDefaultRuleStaysDeletedAcrossARelaunch |
| The seeded rules are ordered FATAL, ERROR, WARN because the priority axis is a minimum | Seeded FATAL/ERROR/WARN (L251) | tst_highlight |
| Each seeded rule carries `Color` alone, so nothing serializes differently and no schema version moves | Seeded FATAL/ERROR/WARN (L251) | tst_highlight |
| The pane docks are tabbed through a cursor (`lastTabbed`), never a written-out chain | Presets are a build option (L253) | tst_panechrome |
| `tst_panechrome` turns on the COUNT of the pane docks as well as their names, so both come from one build-aware helper | Presets are a build option (L253) | tst_panechrome |
| The order is selection → geometry → target line → scroll | Five filter-anchor rules (L257) | tst_logview::theWrappedSelectionIsFoldedInBeforeTheViewIsRepositioned |
| The Runs pane's `Regex`/`Case sensitive` boxes are editors: only Apply and Return apply a pattern | Runs pane applies on Apply (L259) | tst_runpane |
| `updateApplyNote()`'s three-way comparison is load-bearing, not redundant | Runs pane applies on Apply (L259) | tst_runpane |
| `runStartRegex`/`runStartCase` carry object names because their labels are `tr()`'d prose | Runs pane applies on Apply (L259) | tst_runpane |
| The `followLastRunIfMoved()` call sits ABOVE the ingest handler's `ctx != activeContext()` early return | Runs pane opens on Last run (L261) | tst_lastrun |
| The Find status label carries `QSizePolicy::Ignored` plus a stretch share (box 3, status 2) so its wording moves no other control | Find bar status width (L265) | tst_find::theControlsDoNotMoveWhenTheStatusTextChanges |
| `reveal()` must move the focus, or Escape never reaches `FindBar::keyPressEvent` and the bar is closable only with the button | Find reveal precedes report (L267) | tst_find::escapeStillClosesTheBarThatFindNextRevealed |
| Every run of one cell goes into one `QRegion` and the redraw is issued ONCE per cell, not once per match | Mark redraw once per cell (L271) | tst_logview::aMarkedCellIsRedrawnOncePerCellAndNotOncePerMatch |
| The two wrapped-cell renderings must break lines identically; `layoutWrappedText()` lays every wrapped cell out | Wrapped renderings break alike (L273) | tst_logview::aTabbedRecordBreaksWhereItAlwaysDidWhenFindIsArmed<br>tst_logview::theSelectedRecordBreaksWhereItAlwaysDidWhenFindIsArmed |
| The wrap mode still differs per rendering: `AlwaysOn` is `WrapAnywhere`, `SelectedRecordOnly` is `WrapAtWordBoundaryOrAnywhere` | Wrapped renderings break alike (L273) | tst_logview::theSelectedRecordBreaksWhereItAlwaysDidWhenFindIsArmed |
| A tab is laid out as one blank by substituting a space one character for one, so every `TextMatcher::Span` offset still holds | Wrapped renderings break alike (L273) | tst_logview::aTabbedRecordBreaksWhereItAlwaysDidWhenFindIsArmed |
| The Windows read open is `SharedReadFile` — `CreateFileW` with all three share bits, so the writer may still roll or delete the log | Windows share-mode open (L279) | tst_sharedreadfile<br>tst_reload::reloadingAVanishedLogWaitsForItRatherThanFailing |
| `pathIdentity()` on Windows is volume serial plus file index from `GetFileInformationByHandle`, opened with all three share bits | Windows pathIdentity (L283) | tst_pathidentity |
| 0 means unknown, never "replaced"; every caller guards on `current != 0 && captured != 0` | Windows pathIdentity (L283) | tst_pathidentity |
| A ReFS volume's 128-bit index is answered as unknown rather than as a value every file on the volume would share | Windows pathIdentity (L283) | tst_pathidentity |
| The identity is re-resolved from the PATH, never read off the handle the source already holds | Windows pathIdentity (L283) | tst_pathidentity |
| Any test asserting on resolved font properties must guard on an empty `QFontDatabase::families()` and `QSKIP` | Windows has no fonts (L285) | tst_logview::everyColumnRendersFixedPitch |
| Past `kMaxBuckets` buckets are merged pairwise by a bitwise OR over the class bits, never by keeping the lower index | Density map (L93) | tst_densitymap::theCoarseningThatGrowthForcesUnionsTheBucketsItMerges |
| `HostBookmarkStore::find()` keeps comparing the RAW `user` field — a different question from `target()`'s | Password key agreement (L113) | tst_hostbookmarks::findComparesTheUserAsWrittenAndNotTheOneAConnectWouldFillIn |
| A reconnect is held until it has bytes, and the guard sits BEFORE `refreshSize()`, which adopts the new generation | Stale document (L115) | tst_waitingremote::aHeldReconnectLeavesTheCachedRecordsReadableAndNotMerelyCounted |
| `isDelivering()` defaults to TRUE, or every local file is permanently out of reach | Stale document (L115) | tst_spooledsource::aLocalSourceDeliversByDefaultAndASpooledOneOnlyWhileItIsFetching |
| `replaced` is tested BEFORE the shrink, or nearly every real rotation is called a truncation | Rotation announced (L119) | tst_remotetail::aRotationOntoASmallerLogIsAnnouncedAsReplacedAndNeverAsTruncated |
| `reloaded` is emitted BEFORE `rescanned()`, which must stay the last statement of `doRescan()` | Rotation announced (L119) | tst_tail::theReloadCauseIsAnnouncedBeforeTheRescanThatCarriesIt |
| `publishDigest()` dedupes by ordinal and THEN reorders by timestamp; a record with no timestamp keeps its slot and never reaches the comparator | Digest captioned and ordered (L235) | tst_digest::anUnplaceableRecordInTheMiddleKeepsItsSlotRatherThanSortingToTheTop |
| The density marks are NEVER sampled: every row is asked | Density map (L93) | tst_densitymap::aScanIsBoundedByTheBudgetAndResumesWhereItStopped |
| The scan is bounded per SLICE and not in total, on a wall clock rather than a row count | Density map (L93) | tst_densitymap::aScanIsBoundedByTheBudgetAndResumesWhereItStopped<br>tst_densitybar::aHiddenBarScansNothingAndAVisibleOneConvergesWithoutRescanningARow |
| The scan timer runs only while the bar is visible | Density map (L93) | tst_densitybar::aHiddenBarScansNothingAndAVisibleOneConvergesWithoutRescanningARow |
| Buckets hold a fixed ROW COUNT, never a fraction of the view, which is what makes an append free | Density map (L93) | tst_densitymap::growingTheViewNeverRescansWhatWasAlreadyScanned |
| ASCII is filled eagerly and everything else goes through a direct-mapped 4096-slot cache | Wrapped height measured (L157) | tst_wrapmetrics::theMemoMeasuresEachCodepointOnceHoweverOftenItOccurs |
| The wrap memo is per CODEPOINT, never per record | Wrapped height measured (L157) | tst_wrapmetrics::theMemoMeasuresEachCodepointOnceHoweverOftenItOccurs<br>tst_wrapmetrics::aFontChangeDropsTheMemoAndPaysForTheAsciiTableAgain |
| `syncTail()` compares BOTH the record count and the trailing record's display-line count | Block measured in part (L179) | tst_estimatedgeometry::anAppendReMeasuresOnlyTheRecordsTheGrowthCouldHaveTouched |
| A block is measurable in PART — the trailing block's cache is truncated to the old trailing record, never dropped | Block measured in part (L179) | tst_estimatedgeometry::anAppendReMeasuresOnlyTheRecordsTheGrowthCouldHaveTouched<br>tst_logview::aRepaintMeasuresNothingItHasAlreadyMeasuredAndATailMeasuresWhatGrew |
| The Find count is `Find::tally()`, bounded by rows and by wall clock, because counting decodes every visible record on every keystroke | Find reports match of how many (L263) | tst_filter::theMatchTallyCountsWithinItsBoundAndSaysWhenItStoppedShort |
| A repaint measures each block ONCE, so a scroll, a tail tick and a tab switch re-decode nothing | Block measured in part (L179) | tst_logview::aRepaintMeasuresNothingItHasAlreadyMeasuredAndATailMeasuresWhatGrew |
| `WrapMetrics::setFont()` DROPS the memo, every entry in it being that font's | Log text zooms (L177) | tst_wrapmetrics::aFontChangeDropsTheMemoAndPaysForTheAsciiTableAgain |
| The `perf` label is the wall clock and gates nothing; every cost contract that can be stated exactly is COUNTED | perf label (tests/CMakeLists.txt) | tst_wrapmetrics<br>tst_estimatedgeometry<br>tst_densitybar<br>tst_densitymap<br>tst_filter |
| The scan HOLDS its records and tells the model once, at the end — a view follows the tail, so a batch per chunk read as a log being written live | Scan holds its records (L91) | tst_indexcontroller::noRowsAppearUntilTheScanFinishes<br>tst_scanprogress::theViewSaysItIsIndexingAndHoldsItsRecordsUntilItIsDone |
| The publish runs on the CANCELLED path too — "whatever was scanned so far stays usable" is a promise about exactly those records | Scan holds its records (L91) | tst_scanprogress::pressingStopEndsTheScanShort |
| An empty view says it is indexing through `setScanNotice()`, a second string, never the waiting machinery's `m_placeholderText` | Scan holds its records (L91) | tst_scanprogress::theViewSaysItIsIndexingAndHoldsItsRecordsUntilItIsDone |

## Unguarded — 450 rules

CLAUDE.md states these as load-bearing and names no test for them. This table is
a deliverable in its own right: it is the list of decisions that would go quietly
back the way they came. Two columns, not three, deliberately — there is nothing
for the checker to read here, and a placeholder in a Guard column is an invitation
to fill it in with a plausible-looking case rather than a real one.

| Rule | CLAUDE.md |
| --- | --- |
| Nothing may assume a tab widget is a `DocumentView`, and `m_views` no longer carries tab order | M9 tabs (L9) |
| ONE ENUMERATION, TWO RENDERINGS: `refreshRecentFilesMenu()`/`refreshRemoteHostsMenu()` each read their source once and build both the menu and the page | Welcome screen (L11) |
| Refreshing the welcome lists from `updateEmptyState()` instead puts a `hosts.json` parse on eight sites | Welcome screen (L11) |
| The `&&` mnemonic escape must not travel from the menu build to the list build | Welcome screen (L11) |
| A remote row names its HOST and PATH, never its `ssh://` address, hence `MainWindow::openRemoteBookmark(hostName, path)` | Welcome screen (L11) |
| The bookmark is looked up at activation and never captured | Welcome screen (L11) |
| An empty `path` is how a row says the host has no remembered log and routes to the preset dialog | Welcome screen (L11) |
| An empty list's message is a LABEL over the viewport, never a row | Welcome screen (L11) |
| Nothing may ask whether a log is actually there: `logSourcePresence()` is optimistic for a remote address by design | Welcome screen (L11) |
| `m_welcome` is built in the constructor and never lazily; `restoreSession()`'s `ShellOnly` early return calls `updateEmptyState()` | Welcome screen (L11) |
| The failed-restore sentence goes through `setMessage()` and is cleared by `updateEmptyState()`'s non-empty branch | Welcome screen (L11) |
| The remotes section is HIDDEN without SSH where the menu's two items are GREYED, in one `#if !defined(LOFTAIL_HAVE_SSH)` block | Welcome screen (L11) |
| Each list is a fixed height of TEN ROWS, `MainWindow`'s own `kMaxRecentFiles`, coupled with nothing enforcing it | Welcome layout (L11) |
| The list row height is measured from a prototype item and never derived from the font's height | Welcome layout (L11) |
| Each section's button sits left of its list and level with the LIST, not the heading, with the offset measured | Welcome layout (L11) |
| `MainWindow::openFiles()` is the one funnel for the command line, multi-select, a drop and picked archive members | openFiles (L13) |
| `openFile()` returns bool meaning refused-and-reported, never not-there-yet | openFiles (L13) |
| Failures are collected and named in ONE message | openFiles (L13) |
| The loop must stay non-blocking per address, so N tabs go up at once | openFiles (L13) |
| The parsing half lives in `src/ui/CommandLine.h` because `main()` is unreachable from a test | openFiles (L13) |
| `main()`'s one-line mapping from `cmdLine.files().isEmpty()` is the only part no test reaches | Named-file launch (L15) |
| `tabLabelsFor()` gives the bare name plus a bracket holding only what tells it from its neighbours | Tab labels (L17) |
| `prefixedLabelsFor()` is the older rule, kept for the recent-files menu | Tab labels (L17) |
| Labels are recomputed when the SET of open logs changes, in `relabelTabs()`, and cached in `DocumentContext::tabLabel` | Tab labels (L17) |
| Nothing may recompute a label from `updateTabTitles()`, which runs on every ingest tick | Tab labels (L17) |
| `updateTabTitles()` writes `setTabText`/`setTabToolTip` only on a real change | Tab labels (L17) |
| The tab rule groups on `logSourceBareName()`, never the display name | Tab labels (L17) |
| An axis is spent only where it raises the distinct-label count | Tab labels (L17) |
| The path run strips what every member carries off both ends, an archive's two directory spaces stripped separately | Tab labels (L17) |
| A remote-shaped address that did not parse goes through `RemoteLocation::withoutPassword()` before being split | Tab labels (L17) |
| Cost recorded not fixed: a name already containing brackets can equal another group's label | Tab labels (L17) |
| A remote-shaped address that did not parse never goes through `QFileInfo`, whose last component is the whole userinfo | Display name (L19) |
| `TimestampParser` reads `DateFormat::tokens`, never `qtFormat`, which is display-only | Date format (L21) |
| The composites are expanded by `expansionOf()`, and the recursion is one deep | Date format (L21) |
| A code naming nothing the reader can use is matched and dropped, never rejected and never ignored | Date format (L21) |
| The one refusal is `%n`, and its message goes through `specText()` rather than a `tr()` source string | Date format (L21) |
| Month and day names are `\p{L}+`, not three ASCII letters, with English and the system locale both read back | Date format (L21) |
| A literal space in the format absorbs a RUN of spaces | Date format (L21) |
| A format with a month and day but no year takes one from the clock, once at construction, rolling back a year for the future | Date format (L21) |
| `%z` overrides the source zone per record and `%s` ignores it, both still exactly one conversion on the way in | Date format (L21) |
| `kDefaultDateFormat` (strftime) and `kDefaultQtDateFormat` are not interchangeable | Date format (L21) |
| `Field::group` numbers belong to `recordRe`, so the first line is matched a second time where the pattern needs it | recordStartRe (L23) |
| That second match is gated on the pattern needing it and lives in a `std::optional` | recordStartRe (L23) |
| A group `recordStartRe` does capture is still read from the start match | recordStartRe (L23) |
| `recordStartRe` stays the sole boundary decider; `recordRe` is anchored and never widened to reach continuations | recordStartRe (L23) |
| Six autodetect candidates are syslog layouts, so `/var/log/messages` needs no special case | Syslog candidates (L25) |
| `%Q` accepts two spellings, log4cplus `123.456` and rsyslog `123456`, remainder dropped either way | Syslog candidates (L25) |
| Layer 2 never fires on a syslog file and must not be made to, its anchor being the priority vocabulary | Syslog candidates (L25) |
| libssh2 is optional and auto-detected; everything still builds without it | M11 (L29) |
| The archive fetcher is a second fetcher behind the same spool, so archive-over-SSH composes with neither knowing the other | M12 (L31) |
| Addresses continue through the container, so no new scheme and no session schema bump | M12 (L31) |
| `LogSource::isComplete()` stops the watch with no user-facing mode and the follow control untouched | M12 (L31) |
| `ArchiveStream::openNested()` gives a compressed member a second `struct archive`, raw-capable and with no seek callback | Nested member (L33) |
| No addressing rule moved: `archiveCut()` still cuts at the first archive component and the member is still one string | Nested member (L33) |
| `archive_read_support_format_raw()` goes LAST on the nested handle as on the outer one | Nested member (L33) |
| An archive that is itself a member is refused in words rather than chained | Nested member (L33) |
| Several plain `.log` members opening at once was verified NOT to be the fault; do not re-investigate | Members verified (L35) |
| The pane-stash class of bug cannot resurface: on a zero-record document `allChecked()` is true so the key is omitted | Members verified (L35) |
| `SshSession` has two modes and everything above it is unchanged, both answering the same five operations | SFTP fallback (L41) |
| The fallback is chosen by PROBING, never by reading libssh2's error code | SFTP fallback (L41) |
| The error code is consulted only after the probe has also failed | SFTP fallback (L41) |
| SCP was considered and cannot work: no reads at an offset, no re-stat, no handle to fstat | SFTP fallback (L41) |
| Reading a remote log is one `tail` piped into `head` and measuring one is a separate ladder | M16 (L43) |
| The size ladder is stat, then `ls -lnLd`, then `wc -c <`, settled per server | M16 (L43) |
| The `wc` rung is fenced: it will not settle above 8 MB, clamps the poll to 15 s, and errors past 64 MB | M16 (L43) |
| A rung is settled BEHAVIOURALLY: a size is believed only after `readAt(size-1, 1)` delivers a byte | M16 (L43) |
| `pollOnce()`'s `mtime == kUnknownMtime` branch must come BEFORE the `mtime > m_lastMtime` one | Five M16 rules (L45) |
| `ExecSizeProbe`'s read seam binds `Impl::execRead`, not the public `SshSession::readAt` | Five M16 rules (L45) |
| The size proof accepts `>= 1` byte and skips the read for a zero size | Five M16 rules (L45) |
| The ladder re-settles on EVERY `openFile()`, so a rotation gets a fresh choice | Five M16 rules (L45) |
| The stall timer is restarted only when a compare actually runs, never on growth | Five M16 rules (L45) |
| `beginGeneration()` answers whether it worked and every caller aborts on false, in both fetchers | Five M16 rules (L45) |
| `Priming` is published BEFORE `beginGeneration()` and never after | Five M16 rules (L45) |
| `pollOnce()`'s rotation branch publishes `Live` only if `fetchForward()` returned true | Five M16 rules (L45) |
| Each run NAMES the functions that must have passed, because a `QSKIP` is a zero exit status | SSH CI (L47) |
| `$HOME` steers loftail and NOT the ssh client; a shim earlier on `PATH` adds `-F` | SSH CI (L47) |
| Host keys are generated in the container and read back with `docker exec`, not scanned off the port | SSH CI (L47) |
| The harness keeps loftail's own diagnostic log, the only account of what the fetcher did | SSH CI (L47) |
| (c) Sharing the fetcher's live session is rejected and written down as rejected in `SshSessionCache.h` | SSH slow (L49) |
| (c) `cutLoose()` before destroy everywhere, `~SshSession` writing a farewell packet bounded only by the 20 s timeout | SSH slow (L49) |
| (c) `SshConnectHold` is narrowed to the connect, a hold spanning the errand deadlocking the retry | SSH slow (L49) |
| (d) The stream is served on EQUALITY with `readStreamPos`, never `>=` | SSH slow (L49) |
| (d) The stream is dropped at EOF rather than latched exhausted | SSH slow (L49) |
| (e) The fetch chunk is 1 MB in `kSshFetchChunkBytes`, derived rather than tuned, with the buffer `qBound`ed | SSH slow (L49) |
| (f) `compressionFor(Purpose, bool)` is the whole rule and errands pass `userAsked = true` deliberately | SSH slow (L49) |
| (f) The negotiated methods are latched at the handshake and logged per connect | SSH slow (L49) |
| Only what can be decided with NO I/O still fails an open outright | Refusal keeps tab (L53) |
| `reportOpenRefusal()` is the single funnel and the strip is never the status bar | Open notice (L55) |
| A gesture is bracketed by `beginOpenBatch()`/`endOpenBatch()` and the bracket NESTS | Open notice (L55) |
| `restoreSession()` is bracketed too, and `prepareContext()` takes an out-parameter for the reason | Open notice (L55) |
| A render REPLACES and clears the pending list; nothing appends | Open notice (L55) |
| A `QMessageBox` is not available here, `restoreSession()` running before `show()` | Open notice (L55) |
| `LogSource::notReadyYet()` is asked on BYTES, never on a state change | Six M17 rules (L57) |
| `notReadyYet()` excludes `Live`, so an empty remote log opens as an ordinary empty tab | Six M17 rules (L57) |
| It is consulted in exactly two places and never in `checkNow()`'s vanish branch | Six M17 rules (L57) |
| The prime is published ALL AT ONCE, format and encoding settling from the same first bytes | Six M17 rules (L57) |
| Nothing joins a fetcher: `~SourceSpool` retires it to a reaper | Six M17 rules (L57) |
| The keychain rung does not move despite the read now being marshalled | Six M17 rules (L57) |
| A prompt travels to the application thread through `GuiCallGate`, one gate for the process | Call gate (L59) |
| The gate counts ASKERS and not calls, recording the owning `Qt::HANDLE` and letting that thread re-enter | Call gate (L59) |
| The depth counter is the only thing that clears "a call is running" | Call gate (L59) |
| A call that ran inline re-arms the pump on its way out, after the flag is cleared and only at depth 0 | Call gate (L59) |
| File ▸ Reconnect must be able to clear a password-needed refusal | Call gate (L59) |
| "Skip This Host" skips the HOST, not the file | Call gate (L59) |
| `SshConnectHold` serialises the connect per target, keeping one prompt per host | Call gate (L59) |
| Picker text is a case-insensitive substring, or an anchored wildcard when it carries `*` or `?` | Archive picker (L63) |
| The wildcard goes through core's `wildcardToRegex()`, never `QRegularExpression::wildcardToRegularExpression()` | Archive picker (L63) |
| A hidden row is never opened: `accept()` skips `item->isHidden()` | Archive picker (L63) |
| Select All walks the items rather than calling `QTreeWidget::selectAll()` | Archive picker (L63) |
| The filter is driven by `textChanged` and not `returnPressed` | Archive picker (L63) |
| `updateSummary()` says exactly what it said before there was a filter when nothing is hidden | Archive picker (L63) |
| A relative `LogProfile::configPath` resolves against the log's own directory | M22 (L65) |
| `applyProfileToActive()` names every non-format field by hand | Six M22 rules (L67) |
| `m_views` lost its ordering claim; `viewsInTabOrder()` reads the tab bar and `onTabMoved`'s `move` was deleted | Six M22 rules (L67) |
| The bound document is STICKY across an editor page, so ten actions moved to `activePageIsLog()` | Six M22 rules (L67) |
| Find is the deliberate exception and stays enabled on both kinds of page | Six M22 rules (L67) |
| `onCurrentTabChanged()` refreshes the actions unconditionally, outside the `setActiveView()` branch | Six M22 rules (L67) |
| The unsaved-changes prompt runs BEFORE `saveSession()` in `closeEvent` | Six M22 rules (L67) |
| It restores the file's permissions after the `QSaveFile` rename | Config write (L69) |
| A remote config write is IN PLACE when the file exists, never temp-and-rename | Remote config (L71) |
| Short writes are the normal case and the first return is not the whole thing | Remote config (L71) |
| The exec path sends EOF before closing, or `cat` never finishes | Remote config (L71) |
| Existence is its own round trip, an empty file and a missing one being the same empty stdout | Remote config (L71) |
| `ConfigTransfer` runs off the application thread and is abandoned, never joined | Remote config (L71) |
| A restart script runs where the log is, `RestartTarget` deriving all three states | M23 (L75) |
| Completion is `QProcess::finished`, never a stream reaching EOF | Nine M23 rules (L77) |
| `runScript()` keeps stderr and `Impl::runCommand()` still must not | Nine M23 rules (L77) |
| The restart suspends the session timeout and restores it before the close | Nine M23 rules (L77) |
| A running `QProcess` is disowned to a reaper, `~QProcess` on a live child warning, killing and blocking | Nine M23 rules (L77) |
| `LogProfile::operator==` gains a clause for `restartScript` or the setting is silent data loss | Nine M23 rules (L77) |
| No schema version moves: an added key is what a backward read handles | Nine M23 rules (L77) |
| `ARCHIVE` is keyed on `LogAnchor::archived`, never on the member being non-empty | Nine M23 rules (L77) |
| An inapplicable variable is OMITTED rather than set empty, in all three executors | Nine M23 rules (L77) |
| Restart App is live with no script configured and gated on `hasFile`, not `activePageIsLog()` | Nine M23 rules (L77) |
| `m_editor` sits in a frameless `QScrollArea` so the dialog's floor stops growing with the panel | Preferences scroll (L79) |
| The horizontal bar is off and the scroll area's minimum width is seeded from the editor's | Preferences scroll (L79) |
| The dialog still opens at full height where there is room, via `applyInitialHeight()` | Preferences scroll (L79) |
| The regression test states a RELATION, never a pixel count | Preferences scroll (L79) |
| `SPEC.md` §11's "strictly a reader" clause was amended a second time; do not edit the contradiction out | SPEC §11 amendment (L83) |
| The gap is to the row above IN THE TABLE DOING THE ASKING, not the previous record in the file | Six M24 rules (L87) |
| A gap that cannot be stated is EMPTY, never `0` | Six M24 rules (L87) |
| There is deliberately no walk back past a predecessor whose own date did not parse | Six M24 rules (L87) |
| `AxisEditor::rendersSeconds()` must keep EXCLUDING `SincePrevious` | Six M24 rules (L87) |
| No schema version moves for the added enum value, an older binary reading it as As Written | Six M24 rules (L87) |
| `m_timeDisplayActions` is a fixed array indexed by `int(mode)` and is `[6]` now | Six M24 rules (L87) |
| The session restore builds editor pages through `MainWindow::buildConfigTab()`, the same funnel `openConfigAt()` uses | Session v4 (L89) |
| That funnel deliberately neither raises the page nor takes the focus, and keeps a refusal on the strip | Session v4 (L89) |
| An editor entry stores the CONFIG address, never the log it came from | Session v4 (L89) |
| `windowState` is taken for `version >= 3`, not `== kSchemaVersion` | Session v4 (L89) |
| A shrink clears only the last surviving bucket and rewinds the scan to its first row | Density map (L93) |
| A row whose content moves is rewound the same way, both lanes together, and it must stay a rewind not a `clear(Lane)` | Density map (L93) |
| The denominator is `spanLines()`, in one place | Six density rules (L95) |
| A click goes through `setSliderPosition()`, so follow detaches as it does on a drag | Six density rules (L95) |
| The two lanes are invalidated separately, rules and find moving for different reasons | Six density rules (L95) |
| Switching the marks off REPLACES the bar rather than telling it to draw nothing, and is a `recomputeGeometry()` | Six density rules (L95) |
| The find lane's predicate is `LogModel::rowMatchesText()`, shared with `runFind()` rather than copied | Six density rules (L95) |
| The rule lane stores a rule INDEX and not a colour, so a theme change repaints instead of rescanning | Six density rules (L95) |
| (a) Marks and thumb are placed in `trackRect()`, the part level with the viewport, never the whole widget | Bar corrections (L97) |
| (a) `trackRect()` is passed INTO `bandOf()` rather than asked for there, that function running per bucket per column | Bar corrections (L97) |
| (b) Marks are drawn in COLUMNS, one per rule with anything in this log, or a lone ERROR is not drawn at all | Bar corrections (L97) |
| (b) The find lane's column is allocated on the query being ARMED, not on its having matched | Bar corrections (L97) |
| (b) A column is resolved per PIXEL and not per bucket | Bar corrections (L97) |
| (b) `markColour()` passes over a rule colour that cannot be told from the bar's ground (`kMinMarkContrast`) | Bar corrections (L97) |
| (c) Both margins are equal and the left one is measured from the divider, which is not part of the bar's ground | Bar corrections (L97) |
| The bar paints its own ground and thumb, never the style's groove, or the arrow buttons take pixels back | Marks in the bar (L99) |
| The thumb is drawn LAST and translucent, and floored at `kMinThumbPx` | Marks in the bar (L99) |
| `QScrollBar`'s left-button handling is bypassed entirely, never tuned through `SH_ScrollBar_LeftClickAbsolutePosition` | Marks in the bar (L99) |
| `markBand()` in the test skips the rows under the translucent thumb | Marks in the bar (L99) |
| The pane emits a bare copy request and owns the apply; the window owns the enumeration and the picker | Copy highlighters (L101) |
| Nothing was written for persistence and no schema version moves, `adoptRules()` ending in `highlightersChanged()` | Copy highlighters (L101) |
| The zone conversion is CONDITIONAL, so two logs sharing a display zone copy byte for byte | Copy highlighters (L101) |
| The offer is one entry per FILE, deduped by context over `viewsInTabOrder()` | Copy highlighters (L101) |
| Everything that survives `exec()` is a snapshot and the active context is re-checked afterwards | Copy highlighters (L101) |
| The rules are read off the source `Document`, never off pane state | Copy highlighters (L101) |
| Enablement has two call sites and needs both, `updateActionStates()` and `relabelTabs()` | Copy highlighters (L101) |
| The enabled state is `setCanCopyFromAnotherLog()`'s alone and never `updateRuleButtons()`' | Copy highlighters (L101) |
| Waiting lives on the live seam, not in a fourth `LogSource` — what changes is whether there are bytes yet | M13 waiting state (L105) |
| A waiting LOCAL document releases its source; a waiting SPOOLED one keeps it (the spool owns the retrying fetcher) | M13 waiting state (L105) |
| Only refusals decidable with NO I/O still fail an open outright; a transport refusal keeps its tab | M13 waiting state (L105) |
| `originVanished()` is checked AFTER `wasReplaced()` | Two M13 details (L107) |
| The vanish grace period is 2 s of ELAPSED TIME, never a count of checks — the watcher bursts during a rotation | Two M13 details (L107) |
| `BufferedLogSource::originVanished()` uses `QFileInfo::exists()` and never `pathIdentity()`, whose 0 means unknown | Two M13 details (L107) |
| `checkWhileWaiting()` republishes the wait reason beside `publishSourceStatus()` every tick | Waiting reason republished (L109) |
| The republish guard compares against `Document::waitReason()`, never `publishSourceStatus()`'s `m_lastStatusText` | Waiting reason republished (L109) |
| A restated reason must NOT route through `beginWaiting()` — that brackets a `clearIndex()` in a model reset | Waiting reason republished (L109) |
| `Document::restateWaitReason()` writes `m_waitReason` and nothing else — not the index, not `m_lastError` | Waiting reason republished (L109) |
| `waitingChanged(true, …)` now fires twice running, so a receiver reads it as the current sentence and not a transition | Waiting reason republished (L109) |
| `SessionHealth` is a LATCH, never an action — setting it frees and closes nothing | Machine that reboots (L111) |
| The health latch is cleared only by a call that got a whole answer back — a full `connectTo()` or a `statPath()` | Machine that reboots (L111) |
| `readAt()` latches dead only when NOTHING arrived; the classification's default is "still alive" | Machine that reboots (L111) |
| The numeric mirror of libssh2's error codes is `static_assert`ed against the real header | Machine that reboots (L111) |
| `RemoteLocation::toString()` must NOT use `effectiveUser()` — the stored address stays what the user typed | Password key agreement (L113) |
| `target()` still guards an empty effective user, or a machine with no home is keyed `@host:22` | Password key agreement (L113) |
| Stale-vs-wait is chosen on "is there anything to show", never on "is it remote" | Stale document (L115) |
| A LOCAL log is excluded from stale, because a local wait releases its source for invariant #5's reason | Stale document (L115) |
| Stale has NO placeholder — the sentence goes to a per-view strip, the tab mark and the status bar | Stale document (L115) |
| `beginStale()` re-announces only when the sentence changes, it running on every 750 ms tick of an outage | Stale document (L115) |
| `beginWaiting()` calls `endStale()` first — the strip and mark only ever learn from `staleChanged` | Stale document (L115) |
| Leaving stale asks `LogSource::isDelivering()` and never `!originVanished()`, which a fetcher's `Error` also satisfies | Stale document (L115) |
| `isDelivering()` is asked only of a document that is ALREADY stale; going stale stays the vanish branch's decision | Stale document (L115) |
| Waiting still ends on EXISTENCE — a local `stat` cannot tell "not written yet" from "empty for ever" | Format judged on real bytes (L117) |
| `formatSettled()` carries "not judged yet" and is now read on the local path too | Format judged on real bytes (L117) |
| The settle re-enters through the SAME `resumeRequested()` signal the wait uses, never a second wire | Format judged on real bytes (L117) |
| `LiveController::checkNow()` asks on the SIZE, not on growth | Format judged on real bytes (L117) |
| Every judgement above the flag is gated on it — `openWithSettings()` defers the prompt while unsettled | Format judged on real bytes (L117) |
| `resumeOrSettleDocument()` runs its persist/ask/report block only when the resume actually settled something | Format judged on real bytes (L117) |
| There are two reload causes and not three — rewriting in place counts as replacing | Rotation announced (L119) |
| The no-source branch passes `Retry` and announces nothing; only a rescan that SUCCEEDED announces | Rotation announced (L119) |
| The reload announcement is handled BELOW `ctx != activeContext()` — a passing sentence cannot wait on a background tab | Rotation announced (L119) |
| `announceReload()` returns early when the same sentence is already up, or the 5 s timer re-arms for ever | Rotation announced (L119) |
| The rotation notice goes to `statusBar()->showMessage(…, 5000)`, never `m_statusLabel` | Rotation announced (L119) |
| `ArchiveFetcher::start()` publishes `Waiting` and spawns its worker rather than returning false with no container | Archive with no container (L121) |
| `OpenPolicy::Reuse` stays — a watch tick must never become network work | Archive with no container (L121) |
| The container retry is keyed on `logSourcePresence() == Absent`, never on the open merely having failed | Archive with no container (L121) |
| An unreadable container is a refusal that keeps its tab, and `start()` returns TRUE so the tab survives to say so | Archive with no container (L121) |
| The container retry is LOCAL-ONLY, gated explicitly on `RemoteLocation::isRemote()` and not on presence optimism | Archive with no container (L121) |
| The waiting reason names the container through `QFileInfo::fileName()`, not `logSourceDisplayName()` | Archive with no container (L121) |
| `logPathIsWellFormed()` answers FALSE for an archive address that `needsMember()` | Archive with no container (L121) |
| `checkWhileWaiting()` writes a throttled diagnostic when a resume declines while `back` said yes | Archive with no container (L121) |
| `logSourcePresence()` tells "not there" from "there and shut"; `WaitCause` carries `NoAccess` | Presence vs access (L123) |
| `waitCauseFor()` is the single funnel that picks the wait cause | Presence vs access (L123) |
| Presence stays OPTIMISTIC for a remote address — always `Present`, never `Unreadable` | Presence vs access (L123) |
| `SecretStore::available()` is a real round trip, not `QKeychain::isAvailable()`'s link check | Three M14 rules (L127) |
| `rememberSshPassword()` never falls back to the file once `available()` said yes | Three M14 rules (L127) |
| The keychain rung sits BELOW `authenticate()`'s `!prompter` bail — a keychain read can raise an unlock dialog | Three M14 rules (L127) |
| `askPassword()`'s `bool *remember` is actually read, so ticking the box persists something | M14 dangling wires (L129) |
| Context is two ints on the `Document` and one forward pass, not an axis and not a `FilterSet` field | M15 context (L133) |
| The emitter's `inBound()` is every NON-TEXT axis and its `matches()` is the text axis, so context at 0 changes nothing | Context cuts the predicate (L135) |
| An inactive text axis makes context inert with NO gate — there is no enable flag to fall out of step | Context cuts the predicate (L135) |
| "Either side" counts IN-BOUND RECORDS, not ordinals — the leading window is a backward walk stopping at `lastEmitted` | Four M15 rules (L137) |
| Leading context stays a tail append, never a mid-list insert | Four M15 rules (L137) |
| The live pop point and the re-scan start are the SAME row, `Document::contextWindowStart(base, before)` | Four M15 rules (L137) |
| The pop widens only when the provisional record's match status actually FLIPS | Four M15 rules (L137) |
| All five axes are a checkable `QGroupBox` whose title row is the enable control | Filters pane layout (L139) |
| The time editors carry `QSizePolicy::Ignored` PLUS an explicit `setMinimumWidth`, or the pane's floor is ~394 px | Filters pane layout (L139) |
| Every widget the tests reach for has an object name; no test finds a filter widget by visible text | Filters pane layout (L139) |
| The "Others" row is found by `AxisEditor::kOthersRole`, never by its translated label | Filters pane layout (L139) |
| Values are counted from `AxisEditor::kFirstValueRow`, `count()` being one more than the subsystems | Filters pane layout (L139) |
| `setCollapsible()` does not exist — a switched-off axis keeps its body, because what it offers is worth reading | Axes stay expanded (L141) |
| An axis the format cannot fill is LEFT OUT entirely in both panes; the per-pane hide flag is deleted | Axes stay expanded (L141) |
| `setDocument()`'s `setEnabled(false)` stays — it stops a preset forcing an unfillable axis | Axes stay expanded (L141) |
| Nothing in `AxisEditor` is auto-raised; a frameless button reads as a caption | Axes stay expanded (L141) |
| Axis glyphs are not translated and their words live on `setToolTip` AND `setAccessibleName` | Axes stay expanded (L141) |
| The context spinners are labelled with arrows, never `−`/`+`, `+` already meaning "add" in this pane | Context spinner labels (L145) |
| The context row's spacing is set PER GAP, not left uniform | Context spinner labels (L145) |
| `AxisEditor`'s root layout is not flush — `setContentsMargins(6, 4, 6, 4)` | Context spinner labels (L145) |
| View ▸ Clear Filters is enabled from `FilterPane::hasActiveFilters()`, the marker's own question | Three later pane changes (L147) |
| `updateClearFiltersState()` must not consult `activeContext()`, which is not yet rebound | Three later pane changes (L147) |
| `loggerCoversAll` is written only where the name list would be read wrongly, or default state stops round-tripping | Empty list means two things (L151) |
| The three `AxisEditor::ListRule`s are chosen per AXIS from the state, never from which pane hosts the editor | Empty list means two things (L151) |
| `Unstated` ticks nothing AND un-sees what it leaves unticked | Empty list means two things (L151) |
| A restrictive selection loads exactly what its coverage says and must never widen | Empty list means two things (L151) |
| An empty selection stored before the key is read as "nothing had been offered" | Empty list means two things (L151) |
| The regression test must not pump the event loop between the two opens | Empty list means two things (L151) |
| `boundInstant()` reads `m_renderMode`, not the document's current time display mode | Time bound is the instant (L153) |
| `MatchCriteria` stores display-zone WALL CLOCK, so every write to the seconds pair mirrors into the date editors | Time bound is the instant (L153) |
| A `RunSeconds` bound counts from the selected run, so `onRunSelected` calls `refreshTimeBounds()` on both panes | Time bound is the instant (L153) |
| `EstimatedGeometry` is keyed on the wrap width in PIXELS, not a column count | Wrapped height measured (L157) |
| A font change calls `m_wrapMetrics.setFont()` AND `m_estimated.invalidateMeasurements()` unconditionally | Wrapped height measured (L157) |
| A BMP codepoint's overhang comes from `QFontMetricsF::rightBearing()`, never from the bounding rectangle | Wrapped height measured (L157) |
| A zero-advance codepoint contributes nothing, Qt's unset `glyph_metrics_t` reporting x=100000 | Wrapped height measured (L157) |
| A Common/Inherited-script codepoint is measured beside a wide base too and the wider answer kept | Wrapped height measured (L157) |
| `QFontMetricsF::inFont()` cannot detect a fallback face and must not be used as a screen | Wrapped height measured (L157) |
| Paint and tooltip ask `elidedText` of the SAME width, the section's | Elide and tooltip (L161) |
| The tooltip is not the model's `Qt::ToolTipRole`, which `QHeaderView` shows unconditionally | Elide and tooltip (L161) |
| The message column is excluded while it wraps and otherwise answers PER PHYSICAL LINE | Elide and tooltip (L161) |
| The header tooltip arrives on the header's own viewport, which the view event-filters | Elide and tooltip (L161) |
| `m_header` is initialised to `nullptr` in the class body and `eventFilter()` guards on it | Elide and tooltip (L161) |
| The column seed runs EXACTLY four times per view and never on the ingest path | Column width seed (L163) |
| The fourth seed caller is gated on the FORMAT having just settled, not on the resume | Column width seed (L163) |
| `restoreColumnState()` marks every column as user-sized, or the scan-completion seed widens a narrowed one | Column width seed (L163) |
| The `sectionResized` handler excludes the `0` a hide/unhide reports | Column width seed (L163) |
| `charWidth()` and `textWidth()` floor at 8 px per character for an empty font database | Column width seed (L163) |
| "Fit to Contents" is bounded — intern tables, six level words, or at most 400 sampled records | Column width seed (L163) |
| The two header-menu commands are window-owned `QAction`s with object names, acting on the ACTIVE view | Column width seed (L163) |
| The bound adds NO fourth seed caller — it runs inside `seedColumnWidths()`, never from `resizeEvent` | Seed bounded by viewport (L165) |
| A column the user sized is spent from the budget and never reduced; Reset Widths is the escape hatch | Seed bounded by viewport (L165) |
| The bound is inert until `m_viewportLaidOut`, set from `resizeEvent` | Seed bounded by viewport (L165) |
| `lineHeight()` is the scroll unit and the hit-test unit too; nothing may re-derive a row pitch locally | Line pitch (L167) |
| `fontMetrics().lineSpacing()` is not a fix — leading is 0, so it measures identical at every size | Line pitch (L167) |
| The wrap column count floors the DIVISION and keeps the advance fractional (`QFontMetricsF`) | Wrap column rounding (L169) |
| `QFontMetrics::horizontalAdvance()` is not the fix — it is a `qRound` and fails at the same 15 sizes | Wrap column rounding (L169) |
| `qCeil` on the advance is the mirror fault, hanging a blank line off every wrapped record | Wrap column rounding (L169) |
| `minWrapWidth()` is `qCeil(kMinWrapCols * advance)` and must move in the same change | Wrap column rounding (L169) |
| `m_inFollowScroll` is held across the WHOLE font invalidation, not merely the re-anchor's `setValue` | Zoom throws the reader (L175) |
| The follow guard stays a flag and never a `QSignalBlocker`, for the queued-`rangeChanged` reason | Zoom throws the reader (L175) |
| The log font size is ONE application-wide point size in plain `QSettings`, not per view, profile or session | Log text zooms (L177) |
| Only the SIZE moves; the family stays the platform's fixed-pitch face | Log text zooms (L177) |
| The view notices a font change through `QEvent::FontChange` and nothing else | Log text zooms (L177) |
| `applyFontChange()` guards on `m_header`, the constructor setting the font before building it | Log text zooms (L177) |
| `invalidateMeasurements()` runs AFTER the re-seed and unconditionally | Log text zooms (L177) |
| The font re-anchor is the TOP RECORD via `applyDebouncedResize()`'s precedent, not the filter bracket | Log text zooms (L177) |
| `Ctrl`+wheel is REPORTED (`zoomStepRequested`), never acted on by the view; the remainder accumulates | Log text zooms (L177) |
| The three mapping functions bounds-check the cached prefix and reach it through `constFind` | Block measured in part (L179) |
| `m_selWrapCache` is re-measured by ALL THREE live handlers through `measureSelectionWrap()` | Block measured in part (L179) |
| That re-measure goes BEFORE `updateScrollBars()`, and flooring `recordHeightLines()` is not the fix | Block measured in part (L179) |
| Resize, `sectionMoved` and horizontal scroll all call `recomputeGeometry()` — one funnel | Message origin re-measure (L181) |
| A column move does real work in `SelectedRecordOnly` too, so it may not be scoped `if (estimating())` | Message origin re-measure (L181) |
| The `sectionMoved` lambda must NOT `markUserSized()`, which is `sectionResized`'s alone | Message origin re-measure (L181) |
| The scroll re-measure sits inside `dx != 0`, a vertical scroll moving no origin | Message origin re-measure (L181) |
| Only the WIDTH is clamped, never the origin | One wrap width (L183) |
| The scroll range is `qBound`ed into `int`, not cast — the `qint64` total overflows at 21 M records | One wrap width (L183) |
| `Alt+Z` toggles wrap by TRIGGERING the existing mode actions, so persistence and the checkmark follow | Alt+Z wrap toggle (L187) |
| The wrap toggle is two-way, not a three-way cycle through Selected record only | Alt+Z wrap toggle (L187) |
| The three wrap mode actions carry object names, so no test finds them by visible text | Alt+Z wrap toggle (L187) |
| `LogView::mousePressEvent()` takes the LEFT button only, a right press preceding the context-menu event | Pointer selection (L189) |
| A Ctrl press deliberately arms no drag | Pointer selection (L189) |
| The autoscroll timer scrolls through `verticalScrollBar()->setValue()`, never `scrollContentsBy()` | Pointer selection (L189) |
| Autoscroll is stopped by the release, by `hideEvent()` and by `handleModelReset()` | Pointer selection (L189) |
| The selection cases drive real `QMouseEvent`s at the VIEWPORT, not the handlers directly | Pointer selection (L189) |
| Prose is translated; object names, JSON keys, regex fragments, patterns and `priorityName()` are not | Translation split (L193) |
| A core file carries `Q_DECLARE_TR_FUNCTIONS`, never a hand-rolled `tr()` | Translation split (L193) |
| Never identify a widget by its visible text, in `src/` or in `tests/` | No widget by text (L195) |
| `src/core` contains no `QMutex`, `QMutexLocker` or `QWaitCondition` | No Qt mutexes in core (L197) |
| A test whose own load generator needs joining uses `std::thread`, not `QThread::wait()` | No Qt mutexes in core (L197) |
| Nothing of loftail's is ASan-suppressed; `tests/lsan.supp` holds one third-party entry | ASan gates, TSan does not (L199) |
| The abandoned fetcher is parked in a reachable static so that it needs no suppression entry | ASan gates, TSan does not (L199) |
| Sanitizers amplify coverage, they do not create it — neither ASan nor TSan found the call-gate bug | Sanitizers do not create coverage (L201) |
| A `.clang-tidy` holding `Checks: '-*'` is written into the BUILD tree, which is what keeps AUTOMOC's generated TUs out | clang-tidy does not gate (L203) |
| `CXX_CLANG_TIDY` is set per target on `loftail_core`/`loftail_ui`/`loftail`, never through the global `CMAKE_CXX_CLANG_TIDY` | clang-tidy does not gate (L203) |
| The compilation database and the analyzer must be one toolchain — both routes configure with `clang++` | clang-tidy does not gate (L203) |
| The clang-tidy job's `continue-on-error` goes on the JOB and not on a step, or the workflow conclusion stops being `success` | clang-tidy does not gate (L203) |
| The clang-tidy versions differ on purpose and are pinned on both halves (CI 18, dev box 21); names inert on 18 are not to be deleted | clang-tidy does not gate (L203) |
| `bugprone-narrowing-conversions` is kept ON and every narrowing is an explicit `int(...)`, so a NEW one is visible | clang-tidy does not gate (L203) |
| An unknown *config key* in `.clang-tidy` makes clang-tidy 18 discard the whole file and lint with no checks at all | clang-tidy does not gate (L203) |
| Take an autofix's word for nothing that a `#ifdef` or the Qt floor can change underneath it | clang-tidy does not gate (L203) |
| The default format goes through `offerFormat()` like anything else, and `contains("pattern")` beats `isEmpty()` | M18 superseded by M20 (L205) |
| `DefaultFormatStore`'s three-of-seven-fields rule did NOT survive: a node holds everything or nothing | M18 superseded by M20 (L205) |
| Each level holds a complete `LogProfile` and the deepest match supplies ALL of it; levels are never merged field by field | M20 three levels (L207) |
| A per-log entry exists only while it says something its parent does not, through a single write funnel with a change gate | Eight M20 rules (L209) |
| The other half is a sweep, because a write re-tests only the entry it is writing and a pattern edit can silence an entry nothing rewrites | Eight M20 rules (L209) |
| A per-log node stores NO parent link; the parent is derived by running the matcher | Eight M20 rules (L209) |
| The virtual "no matching pattern" parent is gone — absence of a match is a row's position, not a widget-only parent | Eight M20 rules (L209) |
| The wildcard converter is hand-written; `QRegularExpression::wildcardToRegularExpression()`'s opt-out is Qt 6.6, above the 6.4 floor | Eight M20 rules (L209) |
| `LogSettingsStore::migrateLegacy()` runs in the `MainWindow` constructor BEFORE `restoreSession()` | Eight M20 rules (L209) |
| `openFile()`'s prompt gate is gone entirely — every level is checked against the file, including a `--pattern` | Eight M20 rules (L209) |
| The dialog applies nothing: "Apply to current file" only ARMS a request OK carries out, is checkable per node, carries a notice, and stores a `NodeRef` rather than a profile | Eight M20 rules (L209) |
| `LogProfileEditor::profile()` writes its own fields OVER `FormatEditor::settings()`, never before | Eight M20 rules (L209) |
| The comparison deciding an OK re-read is the STORES against the TAB, never before-against-after through `resolvedProfile()` | Preferences OK re-reads (L211) |
| The OK re-read stays BELOW the `applyRequested()` branch as an `else`, or the log is re-read twice with different answers | Preferences OK re-reads (L211) |
| `m_fileFollowsParent` is a memo taken before the parent could move, never a re-test, and is not set for an autodetected seed; every row write goes through `setFileRow()` | Preferences OK re-reads (L211) |
| Only the ACTIVE log is re-read; a background tab keeps its old format until reopened | Preferences OK re-reads (L211) |
| `formatFits()` is a PRESENCE test — one matching record in the sample — and a rate was tried and reverted as untunable | formatFits is presence (L213) |
| An unattended launch whose pattern does not fit blocks on a modal and opens nothing — the product ruling, not an oversight | --pattern wins (L215) |
| An explicitly empty `--pattern` names no format and is the bare launch, deliberately not an error | --pattern wins (L215) |
| Neither the `logsettings.json` `files[]` removal nor the session's filter/highlighter/run removal bumps a schema version; both `load()`s still read what they no longer write | M21 one file per log (L219) |
| A record exists only while it says something its parents do not — `LogFileSettings::reduce()` applied section by section inside `LogFileStore::save()` | Ten M21 rules (L221) |
| `filterStateSaysNothing()` asks whether an axis NARROWS, never whether values are default | Ten M21 rules (L221) |
| An axis added to `MatchCriteria::resolve()`'s `NoOpAxes::Collapse` belongs in `filterStateSaysNothing()` in the same commit | Ten M21 rules (L221) |
| A remove goes map first and slot file second (the save half is guarded; this half is unkillable from outside, `remove()` letting neither step abort the other) | Ten M21 rules (L221) |
| `commitPreferences()` must NOT write `ctx->settings`, or `applySettings()`'s diff sees no change | Ten M21 rules (L221) |
| `runSelectionOf()` answers nothing while the scan is running or a restore is armed, leaving the stored section alone | Ten M21 rules (L221) |
| The Filters pane is GLOBAL and read only for the log it is showing; the write hangs off the pane's debounced `filtersChanged`, never `applyFiltersFor()` | Ten M21 rules (L221) |
| The prune is `commitPreferences()`, once per visit, gated on `LogSettingsTree::operator==` which ignores pattern ids and compares positions in order | Ten M21 rules (L221) |
| Eviction never takes a log open in a tab; `touch()` moves the MRU tick in memory only | Ten M21 rules (L221) |
| `logSettingsKey()` is `absoluteFilePath()`/`normalizeLogPath()` and NEVER `canonicalFilePath()`, or a symlinked log leaves a record its pattern does not claim | One log, one spelling (L223) |
| Two links to one file are two logs here — an accepted cost | One log, one spelling (L223) |
| A canonicalised legacy record is COPIED under the name asked for, never re-keyed | One log, one spelling (L223) |
| The legacy record is pinned for the length of the copy's allocation, and the map is flushed at once | One log, one spelling (L223) |
| `tests/ConfigReset.h`'s `clearLogSettings()` is the one per-case reset helper; the pool is a DIRECTORY, so a single `QFile::remove()` no longer clears it | Settings outlive the tab (L227) |
| `tst_sessiongui` gets the pool cleared and NOT the session, because its cases chain through the session on purpose | Settings outlive the tab (L227) |
| Four actions, each opt-in per rule; first-match-wins becomes per action | M19 actions are a set (L229) |
| `SPEC.md` §11 stays byte-identical despite the notification contradicting it — do not "fix" the spec or remove the feature | §11 contradiction stands (L231) |
| The digest caption is a SIBLING `SectionBox`, never a parent, because the cap is a fraction of `parentWidget()`'s height | Digest captioned and ordered (L235) |
| `HighlightRule::fromJson` tests `contains("actions")`, never the array's emptiness | Six M19 rules (L237) |
| `toJson` omits the actions key for a colour-only rule, so nothing serializes differently and neither store's version moves | Six M19 rules (L237) |
| The tab marker is set ABOVE the `ingested` handler's `ctx != activeContext()` early return | Six M19 rules (L237) |
| The digest republishes when the provisional record changed even if the ordinal list did not | Six M19 rules (L237) |
| The digest index is always active, never `clear()`ed, or the whole log lands in the strip | Six M19 rules (L237) |
| Every counting or lookup site names the view it means — `findChildren<LogView *>("logView")` | Two LogViews per DocumentView (L239) |
| "The last log has gone" is one funnel, `documentsUnbound()`, called from `onViewDestroyed()`'s empty branch and BOTH exits of `closeAllDocuments()` | Notification has no CI cover (L241) |
| `HighlightAction::Color` is derived from the two swatches and has no tick of its own; re-derived on ingest as well as on edit | Rule actions in the table (L243) |
| Each row owns its two pickers carrying a `"ruleRow"` property, and the rebuild starts with `setRowCount(0)`, never `clearContents()` | Rule actions in the table (L243) |
| `m_updating` is saved and restored, never forced false | Rule actions in the table (L243) |
| The enable column is a `CheckCellDelegate`, because Qt lays an item out from the left edge and the default hit area is the 13 px indicator | Rule actions in the table (L243) |
| Every column but the summary is `QHeaderView::Fixed`, measured once from a prototype, or the columns collapse | Rule actions in the table (L243) |
| Every glyph and swatch is painted, never lettered — Windows offscreen resolves no font | Rule actions in the table (L243) |
| `refreshTimeBounds()` writes only `start`/`end`, only where the stored bound was already valid, converting the STORED wall clock | Nothing rewrites a rule (L245) |
| It is keyed on the display ZONE, never on the mode or the seconds baseline | Nothing rewrites a rule (L245) |
| It runs over EVERY rule and not the selected one | Nothing rewrites a rule (L245) |
| `commit()` is called only when a rule actually moved, this being on the ingest path | Nothing rewrites a rule (L245) |
| `AxisEditor::syncTimeEditorKind()` holds `m_populating` itself, saved and restored, guarded in the function and not at its call sites | Nothing rewrites a rule (L245) |
| `AxisEditor::coversAllFor()`: an axis switched off with nothing ticked covers everything | Nothing rewrites a rule (L245) |
| The regression tests assert `doc->highlighters().rules == HighlighterSet::defaults().rules`, never the marker alone | Nothing rewrites a rule (L245) |
| `updateActivity()` runs from `commit()` as well as from the table rebuild | Highlighters marker (L247) |
| An EMPTY rule list is a difference and IS marked, while no document is not | Highlighters marker (L247) |
| Rule ORDER counts, first-match-wins being per action, so a reorder is a difference | Highlighters marker (L247) |
| A swatch's counterpart colour must be the RESOLVED one, not the stored default | Swatch previews the pair (L249) |
| The preview repaint covers both pickers and hangs off the combo's `currentIndexChanged`, never `refreshRow()` | Swatch previews the pair (L249) |
| The `kDefault` entry keeps its struck-through empty tile rather than being previewed | Swatch previews the pair (L249) |
| The seed enters at the two `MainWindow` sites that make a `Document`, never in `Document` itself | Seeded FATAL/ERROR/WARN (L251) |
| No gate is needed for a format with no `%p` — `AbsentField::DoesNotMatch` already refuses to colour | Seeded FATAL/ERROR/WARN (L251) |
| `AtomicJson` stays ungated — `HostBookmarkStore` writes through it too | Presets are a build option (L253) |
| A saved pane layout crosses the presets gate in both directions with no handling; do not clean a stale `presetsDock` out of the blob | Presets are a build option (L253) |
| The filter anchor is expressed in SOURCE ORDINALS, the one coordinate a refilter does not move | Filters do not move the reader (L255) |
| The `beginFilterUpdate()`/`endFilterUpdate()` bracket sits OUTSIDE the reset and only around a filter re-apply | Five filter-anchor rules (L257) |
| `viewRowAtOrAfter()` is a binary search and means nothing over the digest; `popLastVisible()` leaves `m_ascending` alone on purpose | Five filter-anchor rules (L257) |
| The follow guard is a FLAG, never a `QSignalBlocker`, whose `rangeChanged` connection is queued | Five filter-anchor rules (L257) |
| `handleModelReset()` keeps its `m_estimated.clear()`, because `geom()`'s address is stable across `setVisible()` | Five filter-anchor rules (L257) |
| "Last run" is a MODE (`Document::m_followLastRun`) and must not be inferred from `selectedRun() == runs().size() - 1` | Runs pane opens on Last run (L261) |
| `updateRunsAfterAppend()` deliberately does not retarget; the move is `MainWindow::followLastRunIfMoved()`'s, once per tick | Runs pane opens on Last run (L261) |
| The session saves the follow mode by saving no offset at all, so no schema version moves | Runs pane opens on Last run (L261) |
| No runs means the whole file, not an empty view — the mode is inert until a marker turns up | Runs pane opens on Last run (L261) |
| `Tally::complete` false must render as `47+`, never as `47` | Find reports match of how many (L263) |
| `Tally::index` 0 means the scan never reached the hit, so the bar says the count alone and never infers a position from `total` | Find reports match of how many (L263) |
| The wrap is derived from `from`, captured BEFORE `setCurrentRecord(hit)`, or every search reports itself as a wrap | Find reports match of how many (L263) |
| The report goes in the Find bar's own per-view label, never the window status bar which `updateStatus()` rewrites per tick | Find reports match of how many (L263) |
| An empty query answers only a deliberate navigation, split by `runFind()`'s `fromStart`; collapsing the branch breaks one side silently | Find reports match of how many (L263) |
| A search that matched nothing turns the query field red and reports `0 of 0`; `setQueryFailed()` is cleared ONCE at the top of `runFind()` | Find reports match of how many (L263) |
| The red starts from the field's OWN palette, or `ensureReadablePlaceholder()`'s repair is dropped on every failed search | Find reports match of how many (L263) |
| `bad regex` keeps its words as well as the red, because a colour cannot give a reason | Find reports match of how many (L263) |
| Tests read `FindBar::queryFailed()`, never a `QColor`, which would assert on the runner's theme | Find reports match of how many (L263) |
| Find, Find Next and Find Previous are enabled on `hasFile` and nothing else, since a disabled `QAction` swallows its shortcut silently | Find reports match of how many (L263) |
| `Ignored` must NOT be paired with `setMaximumWidth` — that is the Filters pane's context-spinner overlap | Find bar status width (L265) |
| A minimum width sized to the longest wording is not a fix: a `QLabel`'s hint grows past a floor and there is no longest wording | Find bar status width (L265) |
| The status elides `ElideMiddle`, with the full text on the tooltip only when it was actually cut | Find bar status width (L265) |
| `FindBar::status()`, never the label's text, is what the report IS; reading the label asserts on the bar's width | Find bar status width (L265) |
| `runFind()` opens with `FindBar::reveal()` above every branch, so every report lands somewhere visible | Find reveal precedes report (L267) |
| It is `reveal()` and not `activate()`, which begins with `setStatus(QString())` and would replace silence with a blank bar | Find reveal precedes report (L267) |
| `reveal()` is a no-op on a bar already open, tested with `isHidden()` and not `isVisible()` | Find reveal precedes report (L267) |
| Ctrl+F still searches nothing: a reopened bar shows the standing query with a blank report and no marks | Find reveal precedes report (L267) |
| Find cases must assert `isVisible()` as well as `status()` and drive a real `QTest::keyClick`, not `trigger()` the action | Find reveal precedes report (L267) |
| Find marks hold NOTHING: `LogView` keeps the query and re-runs it over the cells it is already painting | Find marks the matched run (L269) |
| Mark positions come off `TextMatcher::spans()` beside `matches()`; a second matcher makes a mark disagree with a hit | Find marks the matched run (L269) |
| The glyphs are redrawn by the same call, clipped, never re-positioned at a measured offset | Find marks the matched run (L269) |
| `drawWrappedCell()` lays out with a `QTextLayout` for EVERY wrapped cell, not only for a record that matches | Find marks the matched run (L269) |
| A mark over an elided cell is computed on the string as drawn, so a match past the ellipsis is not marked | Find marks the matched run (L269) |
| Spans are capped per cell (`kMaxCellMarks`) and zero-width regex matches yield none | Find marks the matched run (L269) |
| The digest strip is never armed — `runFind()` reaches `activeLogView()` only, and the matcher is cleared on every failing branch | Find marks the matched run (L269) |
| Every rectangle is filled before anything is redrawn; a fill after a redraw erases the glyphs of the run it lands on | Mark redraw once per cell (L271) |
| The redraw stays one call over the union, never `QTextLine::draw()` per line, which is not pixel-identical for fallback faces | Mark redraw once per cell (L271) |
| `drawElidedCell()` places runs with a `QTextLayout` and `cursorToX()`, never summed `QFontMetrics::horizontalAdvance()` | Mark redraw once per cell (L271) |
| `measureWrappedLines()` may not go back to `QFontMetrics::boundingRect()`, which measures no word-boundary wrapping | Wrapped renderings break alike (L273) |
| Both wrap-break cases must settle the viewport geometry with a render BEFORE measuring the wrap width | Wrapped renderings break alike (L273) |
| Copy walks the selection as ranges and appends into one reserved `QString`, never a joined list | Copy re-enters the event loop (L275) |
| The copy progress dialog is application-modal and shown BEFORE the first `processEvents()`, not after `minimumDuration` | Copy re-enters the event loop (L275) |
| A model reset abandons the copy; an append or tail removal does not, every row being re-resolved through `sourceRow()` | Copy re-enters the event loop (L275) |
| `setAutoReset(false)`, or `reset()` clears the flag `wasCanceled()` answers and a Cancel on the last chunk is forgotten | Copy re-enters the event loop (L275) |
| The clipboard is written once at the end, which is what makes a cancelled copy leave it untouched | Copy re-enters the event loop (L275) |
| Below `copyProgressThreshold()` there is no dialog and no event processing at all | Copy re-enters the event loop (L275) |
| `BufferedLogSource` keeps its own `m_path`: `wasReplaced()`, `originVanished()` and `computeIdentity()` may not route through the handle | Windows share-mode open (L279) |
| `LogSource::bytes()` takes the caller's storage; no source may keep a read buffer again | bytes() takes caller storage (L281) |
| `SharedReadFile::read()` is positional (`pread`, `OVERLAPPED`), never seek-then-read on a shared handle | bytes() takes caller storage (L281) |
| `bytesCopy()` is for cold callers wanting an owned sample, never a hot path | bytes() takes caller storage (L281) |
| An empty `FONTCONFIG_FILE` run reproduces the Windows box-engine geometry locally, but not the empty-family-list guard | Fontless run on Linux (L287) |
| A test comparing a stored address to a literal spells it `QDir::rootPath() + "var/log/…"`, never an `#ifdef` | POSIX-absolute paths on Windows (L289) |
| Callers write `QString(priorityName(p))`, never `priorityName(p).toUtf8()`, which does not exist on the Qt 6.4 floor | Dev Qt newer than reference (L291) |
| A libssh2 error name newer than the reference (`LIBSSH2_ERROR_MAC_FAILURE`) is `#ifdef`'d, while its mirror value is not | Dev Qt newer than reference (L291) |
| `NOMINMAX` and `WIN32_LEAN_AND_MEAN` are defined PUBLIC on `loftail_core` so every test inherits them past libarchive's header | Windows min/max macros (L293) |
| A Windows test failure is read from the CI diagnostic re-run, not the empty ctest output block | Windows CI diagnostic (L295) |
