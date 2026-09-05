#pragma once

/*
 * base_tools.h — Базовые инструменты AI-кодера (общие для всех модулей).
 *
 * read_file, write_file, search_replace, repo_map, grep_search,
 * rag_index, rag_query, list_skills.
 */

#include "module_api.h"
#include <vector>

namespace coder {

/* Регистрация базовых инструментов в реестре. */
void register_base_tools();

/* Регистрация инструментов RAG (требуют callbacks хоста). */
void register_rag_tools();

} // namespace coder
