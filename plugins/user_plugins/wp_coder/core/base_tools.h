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

/* A1.1: если вывод инструмента превышает порог (то, что компакшн всё равно
 * обрезал бы), пишет полный текст в архив-скоуп и возвращает локатор
 * `session_id:call_id`; иначе — пустую строку (вывод не трогаем). Вызы callers —
 * цикл агента после запуска инструмента; компакшн не трогается. */
std::string archive_output_if_large(const std::string& session_id,
                                    const std::string& call_id,
                                    const std::string& text);

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
