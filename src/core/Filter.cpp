#include "Filter.h"

namespace loftail {

void TextMatcher::set(const QString &pattern, bool regex, Qt::CaseSensitivity cs)
{
    m_pattern = pattern;
    m_regex = regex;
    m_cs = cs;
    m_valid = true;

    if (regex && !pattern.isEmpty()) {
        QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
        if (cs == Qt::CaseInsensitive)
            opts |= QRegularExpression::CaseInsensitiveOption;
        m_re = QRegularExpression(pattern, opts);
        m_valid = m_re.isValid();
        // Speeds repeated matching over millions of records; harmless if it fails.
        if (m_valid)
            m_re.optimize();
    } else {
        m_re = QRegularExpression();
    }
}

bool TextMatcher::matches(const QString &text) const
{
    if (m_pattern.isEmpty())
        return true; // an empty query matches everything (axis treated inactive)
    if (m_regex) {
        if (!m_valid)
            return false; // a broken regex matches nothing rather than throwing
        return m_re.match(text).hasMatch();
    }
    return text.contains(m_pattern, m_cs);
}

int Find::search(int count, int from, bool forward, bool wrap,
                 const std::function<bool(int)> &match)
{
    if (count <= 0 || !match)
        return -1;

    // Start one step past the cursor so Find Next advances; -1 begins at the
    // natural end (top when going forward, bottom when going back).
    int start;
    if (forward)
        start = (from < 0) ? 0 : from + 1;
    else
        start = (from < 0) ? count - 1 : from - 1;

    const int step = forward ? 1 : -1;
    // Visit each of the `count` rows at most once, in order from `start`, wrapping
    // around exactly once when `wrap` is set (SPEC.md §5).
    for (int visited = 0; visited < count; ++visited) {
        int r = start + step * visited;
        if (r < 0 || r >= count) {
            if (!wrap)
                break;
            r %= count;
            if (r < 0)
                r += count;
        }
        if (match(r))
            return r;
    }
    return -1;
}

} // namespace loftail
