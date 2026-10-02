#pragma once

/*
 * base_tools.h — Базовые инструменты AI-кодера (общие для всех модулей).
 *
 * read_file, write_file, search_replace, repo_map, grep_search,
 * rag_index, rag_query, list_skills.
 */

#include "module_api.h"
#include <string>
#include <vector>

namespace coder {

/* Регистрация базовых инструментов в реестре. */
void register_base_tools();

/* Регистрация инструментов RAG (требуют callbacks хоста). */
void register_rag_tools();

/* И9.6: краткий уровень каталога навыков — описание инструмента
 * skill_detail, то есть перечисление ИМЁН.
 *
 * Каталог принимается параметром, а не берётся из SkillsManager внутри:
 * SkillsManager — синглтон, который копится между проверками, и ветка
 * «навыков нет» не была бы достижима никогда. Проверка, которая не может
 * дойти до своего предмета, зелёная вхолостую (то же, что с пустым
 * перечнем в проверке приоритета имён). */
std::string skill_detail_description(const std::vector<Skill>& skills);

} // namespace coder
