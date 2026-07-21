#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace loftail {

// Maps strings to dense quint32 ids so filter predicates compare integers, not
// strings (invariant #4, ARCHITECTURE.md §5). Used for BOTH logger names and
// thread names — thread interning gives thread filtering the same fast path and
// the same free value-set discovery as logger filtering (§5).
//
// The reverse table (`names`) is the authoritative subsystem/thread list shown
// in the filter pane (SPEC.md §6): discovery is a side effect of indexing, not a
// separate pass. Id 0 is reserved for the empty string so a record whose pattern
// lacks that field still has a valid, stable id.
class InternTable
{
public:
    InternTable()
    {
        // Reserve id 0 == "" so "field absent" and "field empty" both map to 0.
        m_names.append(QString());
        m_ids.insert(QString(), 0);
    }

    quint32 intern(const QString &s)
    {
        auto it = m_ids.constFind(s);
        if (it != m_ids.constEnd())
            return it.value();
        const quint32 id = static_cast<quint32>(m_names.size());
        m_ids.insert(s, id);
        m_names.append(s);
        return id;
    }

    // The display string for an id, or empty if out of range.
    QString name(quint32 id) const
    {
        return id < static_cast<quint32>(m_names.size()) ? m_names.at(id) : QString();
    }

    // Resolve a name to its id WITHOUT interning it (const, unlike intern()). Used
    // by the filter pane to turn checked subsystem/thread names into the quint32
    // id sets the predicate compares (invariant #4). A name not yet seen in the
    // file has no id: `*found` is set false and 0 (the empty-string id) returned,
    // so a manually-entered-but-absent value simply matches nothing until it
    // appears. `found` may be null.
    quint32 idOf(const QString &s, bool *found = nullptr) const
    {
        auto it = m_ids.constFind(s);
        const bool has = it != m_ids.constEnd();
        if (found)
            *found = has;
        return has ? it.value() : 0;
    }

    int count() const { return m_names.size(); }
    const QVector<QString> &names() const { return m_names; }

private:
    QHash<QString, quint32> m_ids;
    QVector<QString>        m_names;
};

} // namespace loftail
