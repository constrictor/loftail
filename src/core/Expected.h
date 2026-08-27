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

#include <utility>
#include <variant>

namespace loftail {

// A minimal Result/Either type: it holds either a value of type T (success) or
// an error of type E (failure). C++20 has no std::expected (that arrived in
// C++23) and Qt 6.4 ships nothing equivalent, so this small template stands in.
//
// It is deliberately tiny — just enough for PatternCompiler::compile to return
// "LogFormat or CompileError" without exceptions (ARCHITECTURE.md §3). T and E
// must be distinct types.
template <class T, class E>
class Expected
{
public:
    // Build a success result. Implicit so `return format;` works.
    Expected(T value) : m_storage(std::move(value)) {}

    // Build a failure result. Named, so an error is never created by accident.
    static Expected makeError(E error) { return Expected(std::move(error), ErrorTag{}); }

    bool hasValue() const { return std::holds_alternative<T>(m_storage); }
    explicit operator bool() const { return hasValue(); }

    T &value() { return std::get<T>(m_storage); }
    const T &value() const { return std::get<T>(m_storage); }

    E &error() { return std::get<E>(m_storage); }
    const E &error() const { return std::get<E>(m_storage); }

private:
    struct ErrorTag {};
    Expected(E error, ErrorTag) : m_storage(std::move(error)) {}

    std::variant<T, E> m_storage;
};

} // namespace loftail
