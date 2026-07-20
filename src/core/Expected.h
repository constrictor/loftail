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
