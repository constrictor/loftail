#include "Document.h"

#include "ContextEmitter.h"
#include "IFormatProvider.h"
#include "Indexer.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "PatternCompiler.h"
#include "RemoteLocation.h"
#include "SpooledLogSource.h"
#include "TimestampParser.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <limits>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::Document)
};
} // namespace


Document::Document()
{
    // The FilteredIndex is a view over m_index; its address is stable for the life
    // of the Document (a member), so binding once here is enough even though
    // m_index's contents are reassigned on each prepare()/open().
    m_filtered.setSource(&m_index);
    m_digest.setSource(&m_index); // same reasoning; a second view over the same index
    clearDigest();
}

void Document::clearDigest()
{
    m_digestLast.clear();
    // setVisible({}), never clear(). A cleared FilteredIndex is the IDENTITY view — it
    // reports every source record — which is the right meaning of "no filter" and the
    // exactly wrong meaning of "no digest". The digest is therefore ALWAYS active, from
    // construction onwards, so that "nothing to show" is zero rows and never the whole
    // log. The strip's visibility is driven straight off its model's rowCount(), so an
    // inactive digest would put the entire file in the strip under the file.
    m_digest.setVisible({});
}

Document::~Document() = default;

QString waitingForText(const QString &path, WaitCause cause)
{
    const QString name = logSourceDisplayName(path);
    return cause == WaitCause::Gone
        ? Tr::tr("%1 is no longer there — waiting for it to reappear").arg(name)
        : Tr::tr("%1 has not appeared yet — waiting for it").arg(name);
}

QTimeZone Document::inferSourceZone(const LogFormat &format)
{
    // %d implies UTC, %D implies local (§5.1). Meaningful only when there is a
    // date field; without one the zone is unused.
    if (format.impliedZone == Qt::UTC)
        return QTimeZone::utc();
    return QTimeZone::systemTimeZone();
}

void Document::recomputeDisplayZone()
{
    switch (m_timeDisplay) {
    case TimeDisplay::LocalTime:
        m_displayZone = QTimeZone::systemTimeZone();
        return;
    case TimeDisplay::Utc:
        m_displayZone = QTimeZone::utc();
        return;
    // "As written" performs no conversion at all, i.e. it renders in the zone the
    // text was parsed in. The two seconds modes are zone-free and land here too;
    // the Date column ignores the result, and the other consumers (run labels,
    // filter bounds) get the file's own wall clock, which is the sane fallback.
    case TimeDisplay::AsWritten:
    case TimeDisplay::EpochSeconds:
    case TimeDisplay::RunSeconds:
        break;
    }
    m_displayZone = m_sourceZone;
}

bool Document::prepare(const QString &rawPath,
                       IFormatProvider &provider,
                       Encoding requestedEncoding,
                       const QTimeZone &sourceZone)
{
    // A remote or archived path is reduced to its one spelling HERE, not only at the
    // UI entry point: path() is what the session file stores and what viewOfPath()
    // compares, so the invariant has to hold no matter who opened the Document
    // (RemoteLocation.h).
    const QString path = normalizeLogPath(rawPath);

    m_lastError.clear();
    m_waitReason.clear();
    m_waiting = false;
    m_formatSettled = false;
    m_formatError = CompileError{};
    m_format = LogFormat(); // empty == plain text until the provider succeeds

    // Run selection is per-file; a fresh prepare() starts with no runs. The caller
    // re-applies the file's remembered run-start pattern via setRunStart() after
    // this returns (and after indexing, detectRuns() rebuilds the list).
    m_runStartActive = false;
    m_runStartMatcher = TextMatcher();
    clearIndex();

    m_path = path;
    m_requestedEncoding = requestedEncoding;
    m_sourceZonePinned = sourceZone.isValid();

    // Interactive: a never-yet-connected remote path connects here, which may prompt
    // and may block for the connect timeout (§6.3). A local path never does either.
    QString openError;
    if (!openAndSettleFormat(provider, OpenPolicy::Interactive, &openError)) {
        // Not there is not the same as broken. A well-formed address whose log simply
        // is not available WAITS: the document keeps the path, holds an empty index,
        // and the live seam brings it in the moment it appears (SPEC.md §3, §6.5).
        // Everything else — a malformed address, an archive naming no member, a
        // refused host key, a dependency that is not built in — is still a failure
        // with no document to show.
        //
        // BOTH halves are needed. logSourceAvailable() alone says false for `ssh://`,
        // which names no log and never will, and a tab waiting forever for it would be
        // a typo turned into a hang.
        if (logPathIsWellFormed(path) && !logSourceAvailable(path)) {
            m_sourceZone = sourceZone.isValid() ? sourceZone : inferSourceZone(m_format);
            recomputeDisplayZone();
            enterWaiting(waitingForText(path, WaitCause::NotYet));
            return true; // waiting, not open — see isWaiting()
        }
        // A remote failure phrases itself ("host unreachable", "not built in"); a
        // local one has only the path to report, as before.
        m_lastError = openError.isEmpty() ? Tr::tr("Cannot open file: %1").arg(path)
                                          : openError;
        m_path.clear(); // an open that failed outright leaves no document behind
        return false;
    }

    // Settled BEFORE the waiting check below, not after it. That check returns early,
    // and a caller who PINNED a zone — which every restored document with a saved
    // sourceZone does — would otherwise end up with a default-constructed, invalid
    // QTimeZone rendering its Date column, because resume() re-infers only when the zone
    // was not pinned. Rare when only an unreachable host reached here; universal now
    // that every remote open does.
    m_sourceZone = sourceZone.isValid() ? sourceZone : inferSourceZone(m_format);
    recomputeDisplayZone(); // "as written" tracks whatever source zone just settled

    // A spooled source opens even when its input is not there — the spool is legal and
    // empty, and the fetcher behind it is retrying. That is the remote and archived
    // form of the same wait, and it is the source, not the path, that can say so.
    //
    // notReadyYet() is the same wait one step earlier: the fetcher has not FINISHED
    // ASKING yet, or asked and was refused. A remote open reaches this on its way in,
    // every time, because connecting no longer happens before the document exists
    // (§6.3.3).
    if (m_source->originVanished() || m_source->notReadyYet()) {
        // Prefer the TRANSPORT's own words. "app.log has not appeared yet" is right for
        // a local path we just stat'd and wrong for a remote one, where the log may be
        // sitting there perfectly well and the trouble is reaching it — a host that is
        // down, a login that needs a person, an SFTP subsystem that timed out. Only the
        // fetcher knows which, and saying the wrong one sends the user looking in the
        // wrong place.
        const QString reported = sourceStatusText(*m_source, path);
        enterWaiting(reported.isEmpty() ? waitingForText(path, WaitCause::NotYet) : reported);
        return true;
    }

    return true;
}

// Open m_path and resolve the encoding and the format from the first ~64 KB of it.
// Shared by prepare() and resume() so the two cannot drift about what "settled" means:
// a log that was not there when it was opened settles its format from the bytes that
// eventually arrive, and by exactly the same code that a present one does.
bool Document::openAndSettleFormat(IFormatProvider &provider, OpenPolicy policy, QString *error)
{
    m_source = openLogSource(m_path, policy, error);
    if (!m_source)
        return false;

    // Resolve the encoding by sniffing the first ~64 KB (§6.1). The same sample is
    // handed to the provider — the manual provider ignores it, a detector uses it.
    const qint64 sampleLen = qMin<qint64>(64 * 1024, m_source->size());
    const QByteArrayView sample = sampleLen > 0 ? m_source->bytes(0, sampleLen) : QByteArrayView();
    m_decoder = Decoder::detect(sample, m_requestedEncoding);

    m_formatError = CompileError{};
    auto compiled = provider.formatFor(sample);
    if (compiled) {
        m_format = compiled.value();
    } else {
        // Bad/empty/uncompilable pattern: the file STILL opens with unparsed lines
        // as plain text (SPEC.md §4). The empty LogFormat drives the indexer's
        // plain-text path (every line an Unparsed record); remember the error so
        // the Log Format dialog can point at the offending offset.
        m_formatError = compiled.error();
    }
    // Settled means settled AGAINST REAL BYTES, which is why this is not simply "we got
    // here". A spooled source whose input is not there opens perfectly well and hands
    // back nothing, and a format derived from an empty sample is a guess about a log
    // nobody has seen — so resume() has to do this again when the bytes arrive. An
    // empty LOCAL file gets the same answer for the same reason; nothing reads the flag
    // in that case, because a file that is present is never waiting.
    m_formatSettled = sampleLen > 0;
    return true;
}

void Document::clearIndex()
{
    m_index = RecordIndex();
    m_filtered.clear(); // the previous subset indexed records that no longer exist
    // Same reason, and this is why the digest needs no bookkeeping of its own for a
    // rescan, a rotation or a wait: it clears wherever the filtered subset does.
    clearDigest();
    m_runs.clear();
    m_selectedRun = -1;
    recomputeViewBounds();
    invalidateTimeBaselines();
    m_index.rebuildBlockSums(); // an empty index still has a valid (zero) total
}

void Document::enterWaiting(const QString &reason)
{
    m_waiting = true;
    m_waitReason = reason;
    m_lastError.clear(); // waiting is a state, not a failure; nothing to report
    clearIndex();

    // Release a LOCAL source: there is nothing at the path any more, holding an
    // unlinked inode open pins bytes nobody will read, and invariant #5 says observing
    // a log must not disturb the process producing it.
    //
    // KEEP a spooled one. A SpooledLogSource owns the shared_ptr<SourceSpool>, and the
    // spool owns the fetcher that is retrying the connection — dropping the source here
    // would tear down the very thing doing the waiting, and the log would never come
    // back (§6.5). The spool is also what the fetcher publishes its recovery through.
    if (!logPathIsSpooled(m_path))
        m_source.reset();
}

bool Document::resume(IFormatProvider &provider)
{
    // Reuse, not Interactive: this is reached from the watch tick on the GUI thread,
    // where a connect could block the UI or prompt behind the user's back (§6.3). A
    // spooled document's spool is already live — it is what did the waiting.
    const bool settleFormat = !m_formatSettled;
    QString openError;
    if (settleFormat) {
        if (!openAndSettleFormat(provider, OpenPolicy::Reuse, &openError))
            return false;
        // The zone is inferred from the format's DATE SPECIFIER, so a document that
        // opened into waiting inferred it from an empty format — a guess about a log
        // nobody had seen. Now there is a real format, so re-infer (§5.1). A zone the
        // caller pinned is left exactly as it is.
        if (!m_sourceZonePinned)
            m_sourceZone = inferSourceZone(m_format);
        recomputeDisplayZone();
    } else {
        // The format came from bytes we have already seen; keep it (invariant #3 —
        // the pattern is not recompiled) exactly as rescan() does.
        m_source = openLogSource(m_path, OpenPolicy::Reuse, &openError);
        if (!m_source)
            return false;
    }

    m_waiting = false;
    m_waitReason.clear();

    Indexer indexer(m_format, m_decoder, m_sourceZone);
    m_index = indexer.index(*m_source);
    detectRuns();
    selectNewestRun();
    return true;
}

bool Document::open(const QString &path,
                    IFormatProvider &provider,
                    Encoding requestedEncoding,
                    const QTimeZone &sourceZone)
{
    if (!prepare(path, provider, requestedEncoding, sourceZone))
        return false;

    Indexer indexer(m_format, m_decoder, m_sourceZone);
    m_index = indexer.index(*m_source);
    return true;
}

bool Document::prepare(const QString &path,
                       QStringView pattern,
                       Encoding requestedEncoding,
                       const QTimeZone &sourceZone)
{
    ManualFormatProvider provider(pattern.toString());
    return prepare(path, provider, requestedEncoding, sourceZone);
}

bool Document::open(const QString &path,
                    QStringView pattern,
                    Encoding requestedEncoding,
                    const QTimeZone &sourceZone)
{
    ManualFormatProvider provider(pattern.toString());
    return open(path, provider, requestedEncoding, sourceZone);
}

bool Document::rescan()
{
    // Rotation/truncation (M6, SPEC.md §3): the bytes we indexed are gone or moved,
    // so drop the index and read the file at m_path again from the top — a single
    // forward pass (invariant #9), same format/decoder/zone (invariant #3). The
    // FilteredIndex is bound to &m_index (a stable member), so reassigning the
    // index's contents keeps that binding valid; we only clear its active subset.
    m_filtered.clear();
    clearDigest();
    invalidateTimeBaselines(); // every record is about to be replaced
    // Reuse: this runs from the live watch tick, on the GUI thread, so a rotation must
    // not become a reconnect here. A remote file's spool is shared and already live,
    // which makes this a pointer swap rather than any network work at all (§6.3).
    QString openError;
    m_source = openLogSource(m_path, OpenPolicy::Reuse, &openError);
    if (!m_source) {
        // Caught in the gap between a rotation's rename and its recreate, or the log
        // really has gone. Where it is simply not there, that is a WAIT rather than an
        // error: the live seam brings it back the moment it reappears (§6.5), instead
        // of leaving a null source and an error string nobody reads.
        if (!logSourceAvailable(m_path)) {
            enterWaiting(waitingForText(m_path, WaitCause::Gone));
            return false;
        }
        clearIndex();
        m_lastError = openError.isEmpty() ? Tr::tr("Cannot reopen file: %1").arg(m_path)
                                          : openError;
        return false;
    }

    Indexer indexer(m_format, m_decoder, m_sourceZone);
    m_index = indexer.index(*m_source);
    // The index was rebuilt from fresh content (rotation/truncation): re-detect runs
    // and default to the newest (matches the follow-tail-on-rescan behaviour).
    detectRuns();
    selectNewestRun();
    return true;
}

QString Document::messageText(const Record &rec) const
{
    if (!m_source)
        return QString();
    const QByteArrayView bytes = m_source->bytes(rec.offset, rec.length);
    if (bytes.isEmpty())
        return QString();

    bool hadNl = false;
    const qsizetype firstEnd = m_decoder.lineEnd(bytes, 0, &hadNl);

    // No message field (plain-text / unparsed record): match against the whole
    // decoded record so a text filter still works when the pattern defines no %m
    // (SPEC.md §6). Byte range only — invariant #8 keeps this off raw newline scans.
    if (m_format.msgGroup <= 0) {
        QString whole = m_decoder.decode(bytes);
        while (whole.endsWith(QLatin1Char('\n')) || whole.endsWith(QLatin1Char('\r')))
            whole.chop(1);
        return whole;
    }

    const qsizetype firstContentLen = firstEnd - (hadNl ? m_decoder.unitSize() : 0);
    const QString firstLine =
        m_decoder.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, firstContentLen)));

    // The message is the first line's tail past the record-start prefix (§4:
    // recordStartRe matches exactly up to the message field). Matching that shorter,
    // capture-free prefix — instead of the full-record recordRe with all its groups
    // — is markedly cheaper on the text-filter hot path, which runs once per record
    // that survives the integer axes (invariant #4). firstLine is an owned QString,
    // so match() shares its data (COW) with no deep copy; the QStringView-taking
    // matchView() is Qt 6.5+ only and the target minimum is 6.4 (ARCHITECTURE.md §1).
    const QRegularExpressionMatch sm = m_format.recordStartRe.match(firstLine);
    // No match means an unparsed record (§4): the whole line is its text, which is
    // what data() shows, so the text filter must search the same thing.
    QString value = sm.hasMatch() ? firstLine.sliced(sm.capturedEnd()) : firstLine;

    // The message spans continuation lines (invariant #2): append the rest so a
    // multi-line record's full text is searchable, matching what data() shows.
    if (firstEnd < bytes.size()) {
        QString rest = m_decoder.decode(bytes.sliced(firstEnd));
        while (rest.endsWith(QLatin1Char('\n')) || rest.endsWith(QLatin1Char('\r')))
            rest.chop(1);
        if (!rest.isEmpty())
            value += QLatin1Char('\n') + rest;
    }
    return value;
}

void Document::setContext(int before, int after)
{
    m_contextBefore = qBound(0, before, kMaxContext);
    m_contextAfter = qBound(0, after, kMaxContext);
}

void Document::applyFilters()
{
    if (!m_viewRestricted && !m_filters.anyActive()) {
        // Identity view — nothing to materialize. Deliberately not widened by a
        // configured context: with nothing filtered out there is nothing to be
        // context to, and the identity path must stay allocation-free (M15).
        m_filtered.clear();
        return;
    }

    const int n = m_index.records.size();
    QVector<qint32> visible;
    QVector<quint8> context;
    visible.reserve(n);
    context.reserve(n);

    // One forward pass (invariant #9). Run byte-offset bound first (cheapest), then
    // integer axes, then the message decode — reached ONLY for records the earlier
    // tests let through AND when the text axis is active (invariant #4). The context
    // widening rides on the same pass; with before == after == 0 the emitter reduces
    // to exactly "emit every record acceptsInView() admits", since inContextStream()
    // and matchesTextAxis() are that predicate split in two.
    ContextState st;
    emitWithContext(
        0, n - 1, m_contextBefore, m_contextAfter, st,
        [this](int row) { return inContextStream(m_index.records.at(row)); },
        [this](int row) { return matchesTextAxis(m_index.records.at(row)); },
        [&visible, &context](int row, bool isContext) {
            visible.append(row);
            context.append(isContext ? 1 : 0);
        });

    m_filtered.setVisible(std::move(visible), std::move(context));
}

// --- The highlight digest (M19, ARCHITECTURE.md §7.5.2) ------------------------

namespace {

// Which actions a digest rebuild is asking about. Named once so the fence, the gate
// and the scan cannot drift apart.
constexpr HighlightActions kDigestAction = HighlightAction::Digest;

} // namespace

// Publish m_digestLast as a FilteredIndex: the sorted, deduped set of ordinals it
// names. Returns true when the published subset actually changed — the caller uses
// that to decide whether a model reset is owed, because resetting on every quiet tick
// jolts the strip for nothing.
bool Document::publishDigest(bool force)
{
    QVector<qint32> ordinals;
    ordinals.reserve(m_digestLast.size());
    for (const qint32 row : m_digestLast)
        if (row >= 0)
            ordinals.append(row);
    std::sort(ordinals.begin(), ordinals.end());
    ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());

    // `force` is not belt and braces. setVisible() COPIES each 32-byte Record into the
    // compact index, so a digest row whose record grew a continuation line has the same
    // ordinal and a stale height — and skipping the republish on unchanged ordinals,
    // which is what keeps a quiet tick from jolting the strip, is exactly what would
    // then freeze that row's height for the rest of the session. The live path passes
    // force when the provisional record changed under an ordinal the digest holds.
    if (!force && ordinals == m_digest.visible())
        return false;
    // ALWAYS setVisible, never clear(), even for an empty result — see clearDigest().
    m_digest.setVisible(std::move(ordinals));
    return true;
}

void Document::rebuildDigest()
{
    // fill(), not assign(): QList::assign is Qt 6.6 and the floor is 6.4 (Ubuntu
    // 24.04's stock Qt, §1). fill(value, size) resizes and fills, and predates both.
    m_digestLast.fill(-1, m_highlighters.rules.size());

    // No enabled rule asks for a digest: nothing to scan. Published as an ACTIVE empty
    // subset rather than cleared — see publishDigest() for why an inactive one would
    // put the whole log in the strip.
    if (!m_highlighters.anyEnabled(kDigestAction)) {
        publishDigest();
        return;
    }

    // How many rules are still looking, so the walk can stop the moment they are all
    // answered rather than always paying the fence.
    int outstanding = 0;
    for (int i = 0; i < m_highlighters.rules.size(); ++i) {
        const HighlightRule &r = m_highlighters.rules.at(i);
        if (r.enabled && r.actions.testFlag(HighlightAction::Digest) && r.match.anyActive())
            ++outstanding;
    }

    // Backward from the newest record. Only IN-BOUND records count against the fence,
    // so a log split into runs never scans past the selected run's first record — and
    // a record outside the bound is skipped on one integer comparison.
    int examined = 0;
    for (int row = m_index.records.size() - 1; row >= 0 && outstanding > 0; --row) {
        const Record &rec = m_index.records.at(row);
        if (!inRunBound(rec))
            continue;
        if (++examined > kDigestLookback)
            break;
        // Walking backward, the first time a rule is seen to match IS that rule's last
        // match. Asked per rule rather than through matchActions(), because two digest
        // rules can both want this record and first-match-wins would name only one of
        // them. The memo is shared across the rules so the record decodes at most once.
        std::optional<QString> message;
        for (int i = 0; i < m_digestLast.size() && outstanding > 0; ++i) {
            if (m_digestLast.at(i) >= 0)
                continue;
            if (!m_highlighters.rules.at(i).actions.testFlag(HighlightAction::Digest))
                continue;
            if (!m_highlighters.ruleMatches(i, rec, message,
                                            [this, &rec] { return messageText(rec); }))
                continue;
            m_digestLast[i] = row;
            --outstanding;
        }
    }

    publishDigest();
}

bool Document::updateDigestAfterAppend(int firstNewRow, bool provisionalChanged,
                                       int provisionalRow)
{
    if (!m_highlighters.anyEnabled(kDigestAction)) {
        // A rule may have been switched off since the last rebuild; make sure the
        // strip goes with it rather than freezing on its last content.
        m_digestLast.fill(-1, m_highlighters.rules.size()); // Qt 6.4 floor; see above
        return publishDigest();
    }
    // The rule list changed shape since the last rebuild (a rule added or removed):
    // m_digestLast's indices no longer name the same rules, so nothing incremental is
    // safe. The full scan is the correct answer and the cheap one, since a rule list
    // only changes when the user edits it.
    if (m_digestLast.size() != m_highlighters.rules.size()) {
        rebuildDigest();
        return true;
    }

    bool changed = false;
    // A digest row's RECORD changed even though its ordinal did not — the stale-copy
    // case publishDigest()'s `force` exists for.
    bool staleRow = false;

    // The trailing record was re-read in place. It may have STOPPED matching a rule it
    // was the newest match of — a provisional record can flip either way as its
    // continuation lines arrive — in which case that rule's entry must be re-found
    // behind it rather than left pointing at a record that no longer qualifies.
    if (provisionalChanged && provisionalRow >= 0
        && provisionalRow < m_index.records.size()) {
        const Record &prov = m_index.records.at(provisionalRow);
        for (int i = 0; i < m_digestLast.size(); ++i) {
            if (m_digestLast.at(i) != provisionalRow)
                continue;
            const bool stillMatches =
                inRunBound(prov)
                && m_highlighters.ruleMatches(i, prov,
                                              [this, &prov] { return messageText(prov); });
            if (stillMatches) {
                // The ordinal list has not changed, but the Record behind it has, and
                // FilteredIndex holds a 32-byte COPY — so the strip would render this
                // row at its old height forever. Republish rather than skip.
                changed = true;
                staleRow = true;
                continue;
            }
            m_digestLast[i] = findLastMatchBefore(i, provisionalRow - 1);
            changed = true;
        }
    }

    // Forward over the genuinely new records: each can only push an entry later, so
    // this is O(new records × rules) with the same lazy, memoized decode the paint
    // path uses.
    const int from = qMax(0, firstNewRow);
    for (int row = from; row < m_index.records.size(); ++row) {
        const Record &rec = m_index.records.at(row);
        if (!inRunBound(rec))
            continue;
        std::optional<QString> message;
        for (int i = 0; i < m_digestLast.size(); ++i) {
            if (m_digestLast.at(i) == row)
                continue;
            if (!m_highlighters.rules.at(i).actions.testFlag(HighlightAction::Digest))
                continue;
            if (!m_highlighters.ruleMatches(i, rec, message,
                                            [this, &rec] { return messageText(rec); }))
                continue;
            m_digestLast[i] = row;
            changed = true;
        }
    }

    if (!changed)
        return false;
    publishDigest(staleRow);
    return true;
}

int Document::findLastMatchBefore(int ruleIndex, int fromRow) const
{
    int examined = 0;
    for (int row = qMin(fromRow, m_index.records.size() - 1); row >= 0; --row) {
        const Record &rec = m_index.records.at(row);
        if (!inRunBound(rec))
            continue;
        if (++examined > kDigestLookback)
            break;
        if (m_highlighters.ruleMatches(ruleIndex, rec,
                                       [this, &rec] { return messageText(rec); }))
            return row;
    }
    return -1;
}

bool Document::inRunBound(const Record &r) const
{
    return !m_viewRestricted || (r.offset >= m_viewStart && r.offset < m_viewEnd);
}

bool Document::matchesFilters(const Record &r) const
{
    return m_filters.accepts(r, [this, &r] { return messageText(r); });
}

bool Document::acceptsInView(const Record &r) const
{
    return inRunBound(r) && matchesFilters(r);
}

bool Document::inContextStream(const Record &r) const
{
    return inRunBound(r) && m_filters.acceptsIntegerAxes(r);
}

bool Document::matchesTextAxis(const Record &r) const
{
    // The activity test is not FilterSet::acceptsText()'s to make here: it takes the
    // decoded message by value, so calling it unconditionally would decode every
    // record the integer axes let through even with the axis switched off — exactly
    // the cost invariant #4 exists to avoid.
    if (!m_filters.text.active())
        return true;
    return m_filters.acceptsText(messageText(r));
}

int Document::contextWindowStart(int row, int before) const
{
    if (before <= 0)
        return qMax(0, row);
    int need = before;
    int r = row - 1;
    for (; r >= 0; --r) {
        if (inContextStream(m_index.records.at(r)) && --need == 0)
            break;
    }
    return qMax(0, r);
}

QString Document::recordFirstLine(const Record &rec) const
{
    if (!m_source)
        return QString();
    const QByteArrayView bytes = m_source->bytes(rec.offset, rec.length);
    if (bytes.isEmpty())
        return QString();

    // The run-start regexp matches the WHOLE first line (timestamp, thread, priority,
    // logger, message), not the message tail — so decode the first physical line in
    // full. Byte range only; the Decoder owns line boundaries (invariant #8).
    bool hadNl = false;
    const qsizetype firstEnd = m_decoder.lineEnd(bytes, 0, &hadNl);
    const qsizetype firstContentLen = firstEnd - (hadNl ? m_decoder.unitSize() : 0);
    return m_decoder.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, firstContentLen)));
}

void Document::recomputeViewBounds()
{
    m_viewRestricted = false;
    m_viewStart = std::numeric_limits<qint64>::min();
    m_viewEnd = std::numeric_limits<qint64>::max();
    if (!m_runStartActive || m_selectedRun < 0 || m_selectedRun >= m_runs.size())
        return; // no run splitting, or "all runs" selected

    m_viewStart = m_runs.at(m_selectedRun).startOffset;
    // The run ends where the NEXT run begins; the last run runs to EOF (and beyond,
    // so appended records still belong to it — the "watching the last run" case).
    m_viewEnd = (m_selectedRun + 1 < m_runs.size())
                    ? m_runs.at(m_selectedRun + 1).startOffset
                    : std::numeric_limits<qint64>::max();
    m_viewRestricted = true;
}

void Document::setRunStart(const QString &pattern, bool regex, Qt::CaseSensitivity cs)
{
    m_runStartMatcher.set(pattern, regex, cs);
    m_runStartActive = !pattern.isEmpty();
    detectRuns();
    selectNewestRun();
}

void Document::detectRuns()
{
    m_runs.clear();
    invalidateTimeBaselines(); // the run partition is being rebuilt underneath them
    if (!m_runStartActive || m_runStartMatcher.isEmpty() || !m_runStartMatcher.isValid()) {
        recomputeViewBounds();
        return;
    }

    auto makeRun = [this](int startRecord, bool preamble) {
        const Record &r = m_index.records.at(startRecord);
        Run run;
        run.startRecord = startRecord;
        run.startOffset = r.offset;
        run.startTimestamp = r.timestamp;
        run.firstLine = recordFirstLine(r);
        run.isPreamble = preamble;
        return run;
    };

    const int n = m_index.records.size();
    int firstMarker = -1;
    for (int i = 0; i < n; ++i) {
        if (m_runStartMatcher.matches(recordFirstLine(m_index.records.at(i)))) {
            if (firstMarker < 0) {
                firstMarker = i;
                // Records before the first marker are a leading "preamble" run so no
                // content is unreachable and "all runs" == the whole file.
                if (i > 0)
                    m_runs.append(makeRun(0, /*preamble=*/true));
            }
            m_runs.append(makeRun(i, /*preamble=*/false));
        }
    }

    if (m_selectedRun >= m_runs.size())
        m_selectedRun = m_runs.isEmpty() ? -1 : int(m_runs.size()) - 1;
    recomputeViewBounds();
}

bool Document::updateRunsAfterAppend(int oldRecordCount)
{
    if (!m_runStartActive || m_runStartMatcher.isEmpty() || !m_runStartMatcher.isValid())
        return false;

    const int n = m_index.records.size();
    bool changed = false;
    for (int i = qMax(0, oldRecordCount); i < n; ++i) {
        const Record &r = m_index.records.at(i);
        if (!m_runStartMatcher.matches(recordFirstLine(r)))
            continue;
        // A first marker appearing only now: the records before it form a preamble.
        if (m_runs.isEmpty() && i > 0) {
            const Record &r0 = m_index.records.at(0);
            Run pre;
            pre.startRecord = 0;
            pre.startOffset = r0.offset;
            pre.startTimestamp = r0.timestamp;
            pre.firstLine = recordFirstLine(r0);
            pre.isPreamble = true;
            m_runs.append(pre);
        }
        Run run;
        run.startRecord = i;
        run.startOffset = r.offset;
        run.startTimestamp = r.timestamp;
        run.firstLine = recordFirstLine(r);
        m_runs.append(run);
        changed = true;
    }
    if (changed)
        recomputeViewBounds(); // the selected last run's end may now be finite
    return changed;
}

void Document::selectRun(int index)
{
    m_selectedRun = (index >= 0 && index < m_runs.size()) ? index : -1;
    recomputeViewBounds();
}

void Document::selectRunByStart(qint64 startOffset, qint64 startTimestamp)
{
    int best = -1;
    for (int i = 0; i < m_runs.size(); ++i) {
        if (m_runs.at(i).startOffset == startOffset) {
            best = i;
            break;
        }
    }
    if (best < 0 && startTimestamp != Record::kNoTimestamp) {
        for (int i = 0; i < m_runs.size(); ++i) {
            if (m_runs.at(i).startTimestamp == startTimestamp) {
                best = i;
                break;
            }
        }
    }
    selectRun(best >= 0 ? best : (m_runs.isEmpty() ? -1 : int(m_runs.size()) - 1));
}

void Document::invalidateTimeBaselines() const
{
    m_runBase.clear();
    m_fileBase = Baseline();
    m_runHint = -1;
}

qint64 Document::resolveBaseline(Baseline &b, int from, int end) const
{
    if (b.ts != Record::kNoTimestamp)
        return b.ts; // resolved once; only an invalidation can change it
    if (b.scanned < 0)
        b.scanned = from;
    // Records are only ever APPENDED within a run, so resume where the last attempt
    // gave up: a leading stretch of unparsed lines is walked once in total, not once
    // per painted cell.
    for (; b.scanned < end; ++b.scanned) {
        const qint64 ts = m_index.records.at(b.scanned).timestamp;
        if (ts != Record::kNoTimestamp) {
            b.ts = ts;
            return ts;
        }
    }
    return Record::kNoTimestamp;
}

qint64 Document::runBaseTimestamp(int sourceRow) const
{
    const int n = int(m_index.records.size());
    if (sourceRow < 0 || sourceRow >= n)
        return Record::kNoTimestamp;

    // No run splitting configured: the whole file counts as one run, so the mode
    // stays usable and reads as elapsed time from the log's first record.
    if (m_runs.isEmpty())
        return resolveBaseline(m_fileBase, 0, n);

    if (m_runBase.size() != m_runs.size())
        m_runBase.resize(m_runs.size()); // grows on append; existing memos survive

    const auto endOf = [this, n](int k) {
        return (k + 1 < m_runs.size()) ? m_runs.at(k + 1).startRecord : n;
    };

    // Try the one-entry hint first: LogView paints a contiguous row range, so
    // consecutive cells almost always land in the run the previous cell did and skip
    // the search entirely.
    int i = m_runHint;
    if (i < 0 || i >= m_runs.size()
        || sourceRow < m_runs.at(i).startRecord || sourceRow >= endOf(i)) {
        // Runs ascend by startRecord: take the last one starting at or before the row.
        const auto it = std::upper_bound(m_runs.cbegin(), m_runs.cend(), sourceRow,
                                         [](int row, const Run &r) { return row < r.startRecord; });
        i = int(it - m_runs.cbegin()) - 1;
        if (i < 0)
            return Record::kNoTimestamp; // before the first run; detectRuns() always
                                         // emits a preamble, so this is unreachable
        m_runHint = i;
    }
    return resolveBaseline(m_runBase[i], m_runs.at(i).startRecord, endOf(i));
}

int Document::runRecordCount(int i) const
{
    if (i < 0 || i >= m_runs.size())
        return 0;
    const int end = (i + 1 < m_runs.size()) ? m_runs.at(i + 1).startRecord
                                            : int(m_index.records.size());
    return end - m_runs.at(i).startRecord;
}

void Document::reparseTimestamps(const QTimeZone &sourceZone)
{
    m_sourceZone = sourceZone.isValid() ? sourceZone : inferSourceZone(m_format);
    recomputeDisplayZone();    // "as written" follows the source zone
    invalidateTimeBaselines(); // every timestamp is about to be rewritten

    // No date field, or nothing to read: the source zone is inert (§5.1).
    if (m_format.dateGroup <= 0 || !m_source)
        return;

    const TimestampParser parser(m_format.impliedDateFormat.qtFormat, m_sourceZone);
    const QRegularExpression &re = m_format.recordRe;
    const int unit = m_decoder.unitSize();

    // A pass over the existing index (invariant #10): byte offsets and record
    // boundaries are untouched, so no rescan — only the %d field is re-read and
    // re-parsed. Unparsed records carry no timestamp and are left as-is.
    for (Record &rec : m_index.records) {
        if (rec.timestamp == Record::kNoTimestamp)
            continue; // never matched a date; a zone change cannot give it one

        const QByteArrayView bytes = m_source->bytes(rec.offset, rec.length);
        if (bytes.isEmpty())
            continue;

        bool hadNl = false;
        const qsizetype end = m_decoder.lineEnd(bytes, 0, &hadNl);
        const qsizetype contentLen = end - (hadNl ? unit : 0);
        const QString firstLine =
            m_decoder.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, contentLen)));

        const QRegularExpressionMatch m = re.match(firstLine);
        if (m.hasMatch())
            rec.timestamp = parser.parse(m.capturedView(m_format.dateGroup));
    }
}

} // namespace loftail
