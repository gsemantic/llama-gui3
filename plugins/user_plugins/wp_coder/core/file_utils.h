#pragma once

/*
 * file_utils.h — общие файловые помощники (Фаза 4.2).
 *
 * walk_php/walk_all дублировались в base_tools.cpp и wp_tools.cpp
 * (почти идентичные тела, разные наборы skip-каталогов и расширения).
 * Здесь — единственная реализация рекурсивного обхода.
 */

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

namespace coder {
namespace file_utils {

/* Рекурсивный обход каталога с пропуском каталогов из skip_dirs.
 * extension — фильтр расширений (".php"); пусто — все регулярные файлы.
 *
 * И11.10: should_stop спрашивает «прервал ли пользователь ход» и обрывает
 * обход. Возвращает false, если обход НЕ дошёл до конца (прерван), true —
 * дошёл. Проверка идёт по одной записи каталога, как в fileglob::find
 * (glob.h): обход, который спрашивает раз на каталог, не заметил бы отмены
 * на плоском дереве из сотен тысяч файлов. */
inline bool walk_files(const std::filesystem::path& root,
                       std::vector<std::string>& out,
                       size_t limit,
                       const std::vector<std::string>& skip_dirs,
                       const std::string& extension = "",
                       const std::function<bool()>& should_stop = {}) {
    if (!std::filesystem::exists(root)) return true;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (should_stop && should_stop()) return false;
        const auto& p = it->path();
        /* Тип записи спрашивается ЧЕРЕЗ error_code, а не бросающей
         * перегрузкой — и это не аккуратность, а граница живости.
         *
         * Бросающая `it->is_directory()` идёт в status(), то есть РАЗЫМЫКАЕТ
         * симлинк. На петле (a -> b, b -> a) status() возвращает ELOOP, и
         * бросается filesystem_error. Исключение уходит из обработчика
         * инструмента насквозь: ToolsRegistry::run_output зовёт def.handler без
         * try/catch, дальше его некому ловить, и поток агента падает в
         * std::terminate вместе с приложением. Проверено на живом материале:
         * петля симлинков в дереве роняла весь обзор проекта.
         *
         * Перегрузка с error_code на той же записи вернёт false и поставит
         * код, то есть запись просто не попадёт в выдачу.
         *
         * ГРАНИЦА, которую знать при расширении: запись, тип которой не
         * определился (петля, висячая ссылка, нет прав), ПРОПУСКАЕТСЯ молча,
         * и обход продолжается. Возвращаемое значение при этом остаётся
         * «дошёл до конца»: висячая ссылка — обычное дело в настоящем
         * проекте (остатки node_modules, worktree-ссылки .git), и объявлять
         * обзор неполным из-за каждой ссылки значило бы кричать о проблеме
         * там, где её нет. Настоящий обрыв обхода — это предел или отмена,
         * и оба объявляются. */
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            std::string name = p.filename().string();
            if (std::find(skip_dirs.begin(), skip_dirs.end(), name) != skip_dirs.end()) {
                it.disable_recursion_pending();
                continue;
            }
        }
        std::error_code file_ec;
        if (it->is_regular_file(file_ec) &&
            (extension.empty() || p.extension() == extension)) {
            out.push_back(p.string());
            if (out.size() >= limit) return true;
        }
    }
    return true;
}

} // namespace file_utils
} // namespace coder