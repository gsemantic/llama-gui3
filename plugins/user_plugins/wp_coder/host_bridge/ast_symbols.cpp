// ast_symbols.cpp — вызов поля хоста llm_ast_symbols (И13.5).

#include "ast_symbols.h"

#include <cstddef>

namespace coder {
namespace host {

std::string call_ast_symbols(const LlamaHostApi* api, LlamaPluginHost* host,
                             const std::string& path,
                             const std::string& language)
{
    /* Проба поля — руками, как в llm_stream_probe.cpp, и по той же
     * причине: аргументы вычисляются ДО входа, поэтому передать сюда
     * api->llm_ast_symbols нельзя — поле прочиталось бы раньше, чем
     * проверена длина структуры, и на старом хосте это чтение за его
     * пределами. */
    if (!has_llm_ast_symbols(api)) return std::string();
    if (path.empty()) return std::string();

    char* raw = api->llm_ast_symbols(host, path.c_str(),
                                     language.empty() ? nullptr : language.c_str());
    if (!raw) return std::string();

    /* Копия до освобождения: строка хоста живёт только до free_string, а
     * возвращать её дальше по std::function — значит вернуть висящий
     * указатель в разобранном виде. */
    std::string out(raw);
    if (api->free_string) api->free_string(host, raw);
    return out;
}

} // namespace host
} // namespace coder
