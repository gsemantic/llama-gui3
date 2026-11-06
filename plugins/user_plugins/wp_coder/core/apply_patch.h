#pragma once

/*
 * apply_patch.h — патч в формате opencode/codex (И4.2).
 *
 * Формат (порт tool/apply_patch.txt):
 *
 *   *** Begin Patch
 *   *** Add File: src/new.php
 *   +<?php
 *   +echo 1;
 *   *** Update File: src/old.php
 *   *** Move to: src/new_name.php
 *   @@ function wp_footer
 *    echo "старое";
 *   -echo "убираем";
 *   +echo "добавляем";
 *   *** End of File
 *   *** Delete File: src/gone.php
 *   *** End Patch
 *
 * Одна операция — несколько файлов: агент в opencode правет их одним
 * вызовом, и это же нужно здесь. Отдельный инструмент на «замену в
 * файле» (search_replace) остаётся, потому что у него другая семантика
 * (уникальное совпадение), но многофайловые изменения — это именно патч.
 *
 * Внутренняя модель Hunk { old_lines, new_lines, change_context,
 * is_end_of_file } — как в оригинале. Смысл полей:
 *   old_lines — контекст + удаляемые строки: что искать в файле;
 *   new_lines — контекст + добавляемые: что записать на их место;
 *   change_context — текст после @@ («function wp_footer»). Это не
 *     украшение: при двух одинаковых кусках в файле он указывает, после
 *     какого искать, иначе правка попала бы не туда;
 *   is_end_of_file — «*** End of File»: старые строки обязаны быть в
 *     самом конце файла, а при пустом old_lines это «дописать в конец».
 *     Без него «добавить строку в конец» невыразимо: искать нечего.
 *
 * Разбор и применение разведены намеренно: разбор — чистая функция от
 * текста (тестируется без файловой системы), применение хуков — чистая
 * функция от TextFile. Файловую систему и разрешения трогает только
 * инструмент, потому что запись обязана пройти через
 * ToolContext::propose_write (режим плана) и PermissionGate.
 */

#include "text_edit.h"

#include <string>
#include <vector>

namespace coder {
namespace patch {

enum class Op { Add, Update, Delete };

struct Hunk {
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    std::string change_context;
    bool is_end_of_file = false;
};

struct FilePatch {
    Op op = Op::Update;
    std::string path;
    /* Непусто для «*** Move to:» — переименование вместе с правкой. */
    std::string move_to;
    std::vector<Hunk> hunks;
};

struct Parsed {
    std::vector<FilePatch> files;
    /* Непусто — патч не разобран. files при этом пуст: применять половину
     * патча нельзя, это хуже, чем отказ. */
    std::string error;
    bool ok() const { return error.empty(); }
};

/* Разобрать текст патча. Текст после «*** End Patch» — ошибка: модель,
 * которая дописала патч и оборвалась, не должна получить «успех». */
Parsed parse(const std::string& text);

struct Applied {
    bool ok = false;
    /* Понятное объяснение: какой хук и что в нём искалось. */
    std::string error;
    /* Индекс неудачного хука (0-based), для сообщения инструмента. */
    size_t failed_hunk = 0;
    TextFile file;
};

/* Применить хуки к содержимому файла. Уже применённые хуки не
 * откатываются: при ошибке ok = false, а file — промежуточное
 * состояние, и вызывающий обязан его не записывать. */
Applied apply_hunks(const TextFile& in, const std::vector<Hunk>& hunks);

} // namespace patch
} // namespace coder
