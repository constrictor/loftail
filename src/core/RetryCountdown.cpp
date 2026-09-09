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

#include "RetryCountdown.h"

#include <QCoreApplication>

#include <chrono>

namespace loftail {

namespace {
// Nothing in src/core is a QObject, so the translation context is declared rather than
// inherited (ARCHITECTURE.md §9.1).
class Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::RetryCountdown)
};
} // namespace

qint64 fetchMonotonicMs()
{
    static const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

QString retryCountdownText(const QString &reason, qint64 retryAtMs)
{
    if (reason.isEmpty() || retryAtMs <= 0)
        return reason;
    const qint64 remaining = retryAtMs - fetchMonotonicMs();
    if (remaining <= 0)
        return reason;
    return Tr::tr("%1 (%2)").arg(reason).arg((remaining + 999) / 1000);
}

} // namespace loftail
