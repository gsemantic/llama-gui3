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

namespace coder {
namespace file_utils {

/* Рекурсивный обход каталога с пропуском каталогов из skip_dirs.
 * extension — фильтр расширений (".php"); пусто — все регулярные файлы. */
inline void walk_files(const std::filesystem::path& root,
                       std::vector<std::string>& out,
                       size_t limit,
                       const std::vector<std::string>& skip_dirs,
                       const std::string& extension = "") {
    if (!std::filesystem::exists(root)) return;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        if (it->is_directory()) {
            std::string name = p.filename().string();
            if (std::find(skip_dirs.begin(), skip_dirs.end(), name) != skip_dirs.end()) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (it->is_regular_file() &&
            (extension.empty() || p.extension() == extension)) {
            out.push_back(p.string());
            if (out.size() >= limit) return;
        }
    }
}

} // namespace file_utils
} // namespace coder