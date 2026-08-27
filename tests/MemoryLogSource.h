// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "LogSource.h"

#include <QByteArray>

// A LogSource backed by an in-memory buffer, for indexer/model tests that must
// run without touching the filesystem or a QApplication. Exercises the same
// LogSource interface the real sources implement.
class MemoryLogSource final : public loftail::LogSource
{
public:
    explicit MemoryLogSource(QByteArray data) : m_data(std::move(data)) {}

    QByteArrayView bytes(qint64 offset, qint64 length) override
    {
        if (offset < 0 || length <= 0 || offset >= m_data.size())
            return {};
        const qint64 clamped = qMin<qint64>(length, m_data.size() - offset);
        return QByteArrayView(m_data.constData() + offset, clamped);
    }
    qint64 size() const override { return m_data.size(); }
    qint64 refreshSize() override { return m_data.size(); }
    bool isRandomAccess() const override { return true; }
    quint64 identity() const override { return 1; }
    bool wasTruncated() const override { return false; }

private:
    QByteArray m_data;
};
