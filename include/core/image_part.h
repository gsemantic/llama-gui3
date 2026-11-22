#pragma once

/*
 * image_part.h — одно изображение во вложении к реплике.
 *
 * Живёт в ОТДЕЛЬНОМ заголовке, а не в llama_interface.h и не в
 * openrouter_types.h, по одной причине: оба этих заголовка — вход в свой
 * слой (локальный сервер и облако), и ImagePart нужен ОБОИМ. Объяви я его
 * в llama_interface.h, облачный слой потянул бы за собой nlohmann и curl;
 * объяви в openrouter_types.h — локальный потянул бы клиент OpenRouter.
 * Двенадцать строк без зависимостей разрывают эту связь.
 *
 * url — ГОТОВЫЙ data-URL (`data:image/png;base64,...`) либо http(s)-ссылка,
 * в том виде, в каком он уйдёт провайдеру. Два разных ответа на вопрос
 * «откуда взять байты» намеренно не различаются по строке с префиксом:
 * иначе каждый потребитель помнил бы, как их различать.
 *
 * media_type — MIME для локального llama-server. Пустой — провайдер
 * определит тип сам (llama-server это умеет, когда url уже data-URL).
 */

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

struct ImagePart {
    std::string url;
    std::string media_type;
};

} // namespace core
} // namespace llama_gui