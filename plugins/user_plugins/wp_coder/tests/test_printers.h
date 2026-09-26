#pragma once

/*
 * test_printers.h — печать перечислений в ASSERT_EQ.
 *
 * Отдельный заголовок по конкретной причине: ASSERT_EQ печатает оба
 * значения через operator<<, и если объявить оператор в двух файлах
 * тестов, линкер получит определение символа дважды. Именно так и было:
 * operator<< для ToolState появился в test_message.cpp, и
 * test_session_store.cpp с ним не собрался.
 *
 * Все операторы inline — это header, включаемый в несколько
 * единиц трансляции.
 */

#include <ostream>

#include "../core/llm_event.h"
#include "../core/message.h"

namespace coder {

inline std::ostream& operator<<(std::ostream& os, LlmEventKind kind) {
    return os << llm_event_kind_name(kind);
}

inline std::ostream& operator<<(std::ostream& os, PartKind kind) {
    return os << part_kind_name(kind);
}

inline std::ostream& operator<<(std::ostream& os, ToolState state) {
    return os << tool_state_name(state);
}

} // namespace coder
