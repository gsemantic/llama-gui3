#include "../include/core/rag_manager.h"
#include "../include/core/stemmer.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <unordered_set>
#include <unordered_map>

namespace llama_gui {
namespace core {

// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ===

// === ТРАНСЛИТЕРАЦИЯ: Русский → Английский ===
// Используем std::string ключи для поддержки UTF-8
static const std::unordered_map<std::string, std::string> ru_to_en_translit = {
    {"а", "a"}, {"б", "b"}, {"в", "v"}, {"г", "g"}, {"д", "d"},
    {"е", "e"}, {"ё", "yo"}, {"ж", "zh"}, {"з", "z"}, {"и", "i"},
    {"й", "y"}, {"к", "k"}, {"л", "l"}, {"м", "m"}, {"н", "n"},
    {"о", "o"}, {"п", "p"}, {"р", "r"}, {"с", "s"}, {"т", "t"},
    {"у", "u"}, {"ф", "f"}, {"х", "kh"}, {"ц", "ts"}, {"ч", "ch"},
    {"ш", "sh"}, {"щ", "sch"}, {"ъ", ""}, {"ы", "y"}, {"ь", ""},
    {"э", "e"}, {"ю", "yu"}, {"я", "ya"},
    {"А", "A"}, {"Б", "B"}, {"В", "V"}, {"Г", "G"}, {"Д", "D"},
    {"Е", "E"}, {"Ё", "Yo"}, {"Ж", "Zh"}, {"З", "Z"}, {"И", "I"},
    {"Й", "Y"}, {"К", "K"}, {"Л", "L"}, {"М", "M"}, {"Н", "N"},
    {"О", "O"}, {"П", "P"}, {"Р", "R"}, {"С", "S"}, {"Т", "T"},
    {"У", "U"}, {"Ф", "F"}, {"Х", "Kh"}, {"Ц", "Ts"}, {"Ч", "Ch"},
    {"Ш", "Sh"}, {"Щ", "Sch"}, {"Ъ", ""}, {"Ы", "Y"}, {"Ь", ""},
    {"Э", "E"}, {"Ю", "Yu"}, {"Я", "Ya"}
};

static std::string transliterate_ru_to_en(const std::string& text) {
    std::string result;
    result.reserve(text.size() * 2);  // Резервируем с запасом
    
    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        
        // ASCII символ
        if (c < 128) {
            result += c;
            i++;
        }
        // UTF-8 символ (русская буква - 2 байта)
        else if (c >= 0xC0 && c < 0xE0) {
            // 2-байтный UTF-8 символ
            std::string utf8_char = text.substr(i, 2);
            
            // Ищем в таблице транслитерации
            auto it = ru_to_en_translit.find(utf8_char);
            if (it != ru_to_en_translit.end()) {
                result += it->second;
            } else {
                result += utf8_char;  // Оставляем как есть если не найдено
            }
            
            i += 2;
        }
        // 3-байтный UTF-8 символ
        else if (c >= 0xE0 && c < 0xF0) {
            std::string utf8_char = text.substr(i, 3);
            auto it = ru_to_en_translit.find(utf8_char);
            if (it != ru_to_en_translit.end()) {
                result += it->second;
            } else {
                result += utf8_char;
            }
            i += 3;
        }
        // 4-байтный UTF-8 символ
        else if (c >= 0xF0 && c < 0xF8) {
            std::string utf8_char = text.substr(i, 4);
            auto it = ru_to_en_translit.find(utf8_char);
            if (it != ru_to_en_translit.end()) {
                result += it->second;
            } else {
                result += utf8_char;
            }
            i += 4;
        }
        else {
            i++;
        }
    }
    
    return result;
}

// Приведение кириллицы к нижнему регистру (UTF-8)
static std::string to_lower_utf8(const std::string& text) {
    std::string result = text;
    for (size_t i = 0; i + 1 < result.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(result[i]);
        if (c == 0xD0) {
            unsigned char d = static_cast<unsigned char>(result[i + 1]);
            if (d >= 0x90 && d <= 0x9F) {
                result[i + 1] = d + 0x20;      // А..П → а..п
            } else if (d >= 0xA0 && d <= 0xAF) {
                result[i] = 0xD1;
                result[i + 1] = d - 0x20;      // Р..Я → р..я
            } else if (d == 0x81) {
                result[i] = 0xD1;
                result[i + 1] = 0x91;          // Ё → ё
            }
            ++i;
        }
    }
    return result;
}

// Косинусная близость между векторами
static float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a == 0 || norm_b == 0) return 0.0f;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// Токенизация текста (с поддержкой UTF-8 для русских букв)
static std::vector<std::string> tokenize_text(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current_token;

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        
        // Проверяем, является ли символ буквой или цифрой
        bool is_word_char = false;
        
        // ASCII буквы и цифры
        if (c < 128) {
            is_word_char = std::isalnum(c);
        }
        // UTF-8 символы (русские буквы и другие Unicode)
        else if (c >= 0xC0) {
            // Считаем UTF-8 символ буквой (для простоты)
            is_word_char = true;
        }
        
        if (is_word_char || c == '-') {
            // Добавляем символ к текущему токену (обрабатываем многобайтные UTF-8)
            if (c < 128) {
                current_token += c;
                i++;
            } else {
                // Многобайтный UTF-8 символ
                int bytes = 0;
                if ((c & 0xE0) == 0xC0) bytes = 2;
                else if ((c & 0xF0) == 0xE0) bytes = 3;
                else if ((c & 0xF8) == 0xF0) bytes = 4;
                
                for (int b = 0; b < bytes && i + b < text.size(); b++) {
                    current_token += text[i + b];
                }
                i += bytes;
            }
        } else {
            // Конец токена
            if (!current_token.empty()) {
                tokens.push_back(to_lower_utf8(current_token));
                current_token.clear();
            }
            i++;
        }
    }

    if (!current_token.empty()) {
        tokens.push_back(to_lower_utf8(current_token));
    }

    return tokens;
}

// Извлечение ключевых слов из запроса (существительные, имена собственные)
static std::vector<std::string> extract_keywords(const std::string& query) {
    std::vector<std::string> keywords;

    // Расширенные стоп-слова для русского и английского
    static const std::unordered_set<std::string> stop_words = {
        // Русские вопросительные и служебные
        "что", "как", "где", "когда", "почему", "зачем", "какой", "какая", "какое", "какие",
        "кто", "куда", "откуда", "зачем", "почему",
        "в", "на", "о", "об", "по", "для", "от", "до", "из", "с", "к", "у", "за", "под", "над",
        "и", "или", "но", "а", "же", "ли", "бы",
        "этот", "эта", "это", "эти", "тот", "та", "то", "те",
        "весь", "вся", "все", "всё", "сам", "сама", "само", "сами",
        // Русские глаголы-связки
        "сказано", "говорится", "пишут", "документе", "тексте", "документ", "текст",
        "есть", "был", "была", "было", "были", "будет", "будут",
        "тогда", "можно", "нужно", "надо",
        "про", "о", "об", "насчет",
        // Английские вопросительные и служебные
        "what", "how", "where", "when", "why", "which", "who", "whom", "whose",
        "the", "a", "an", "is", "are", "was", "were", "be", "been", "being",
        "have", "has", "had", "do", "does", "did", "will", "would", "could", "should",
        "in", "on", "at", "for", "to", "from", "of", "with", "by", "about", "into",
        "and", "or", "but", "if", "then", "than", "so", "as",
        "this", "that", "these", "those", "all", "each", "every", "both",
        // Английские глаголы-связки
        "said", "written", "document", "text", "about", "regarding"
    };

    auto tokens = tokenize_text(query);

    for (const auto& token : tokens) {
        if (token.length() > 2 && stop_words.find(token) == stop_words.end()) {
            keywords.push_back(token);
        }
    }

    return keywords;
}

// === QUERY EXPANSION: Автоматическое добавление синонимов ===

std::vector<std::string> RagManager::expand_query(const std::string& query) {
    std::vector<std::string> expanded;

    // Добавляем оригинальный запрос
    expanded.push_back(query);

    if (!enable_query_expansion_) {
        return expanded;
    }

    // Извлекаем ключевые слова
    auto keywords = extract_keywords(query);

    // Для каждого ключевого слова добавляем:
    // 1. Транслитерацию (русский → английский)
    // 2. Стеммированные варианты (Snowball-стемминг)
    // 3. Морфологические варианты
    
    for (const auto& keyword : keywords) {
        std::string lower_keyword = keyword;
        std::transform(lower_keyword.begin(), lower_keyword.end(),
                      lower_keyword.begin(), ::tolower);

        // 1. ТРАНСЛИТЕРАЦИЯ: Русский → Английский
        std::string translit = transliterate_ru_to_en(lower_keyword);
        if (translit != lower_keyword) {
            expanded.push_back(translit);
            // Также добавляем с большой буквы для поиска по началу предложения
            if (!translit.empty()) {
                translit[0] = std::toupper(static_cast<unsigned char>(translit[0]));
                expanded.push_back(translit);
            }
        }

        // 2. СТЕММИРОВАННЫЕ ВАРИАНТЫ (Snowball-стемминг)
        std::string stem = Stemmer::stem(lower_keyword);
        if (stem != lower_keyword && stem.length() >= 3) {
            expanded.push_back(stem);
        }

        // 3. МОРФОЛОГИЧЕСКИЕ ВАРИАНТЫ
        auto variants = Stemmer::expand_variants(lower_keyword);
        for (const auto& variant : variants) {
            if (variant != lower_keyword && variant != stem) {
                expanded.push_back(variant);
            }
        }
    }

    return expanded;
}

// === KEYWORD BOOST: Усиление за счёт ключевых слов ===

float RagManager::keyword_boost_score(const RagChunk& chunk,
                                       const std::vector<std::string>& keywords) {
    if (keywords.empty()) {
        return 1.0f;
    }

    std::string chunk_lower = to_lower_utf8(chunk.content);

    int matches = 0;
    int total_weight = 0;
    int best_match_length = 0;

    for (const auto& keyword : keywords) {
        std::string keyword_lower = to_lower_utf8(keyword);

        // Пропускаем очень короткие ключевые слова (< 3 символов)
        if (keyword_lower.length() < 3) {
            continue;
        }

        // Стем ключевого слова для учёта морфологии (омар/омары/омарами)
        std::string stem = Stemmer::stem(keyword_lower);

        // 1. ТОЧНОЕ ВХОЖДЕНИЕ (наибольший вес)
        size_t pos = chunk_lower.find(keyword_lower);
        if (pos != std::string::npos) {
            matches++;
            total_weight += 3;  // Точное вхождение
            
            // Дополнительный вес за начало предложения/заголовка
            if (pos == 0 || !std::isalpha(static_cast<unsigned char>(chunk_lower[pos - 1]))) {
                total_weight += 2;  // Начало предложения или заголовок
            }
            
            // Запоминаем длину лучшего совпадения
            if (static_cast<int>(keyword_lower.length()) > best_match_length) {
                best_match_length = keyword_lower.length();
            }
        } else if (stem.length() >= 3 && stem != keyword_lower) {
            // 1b. МОРФОЛОГИЧЕСКОЕ СОВПАДЕНИЕ (стем: омарами -> омар)
            size_t stem_pos = chunk_lower.find(stem);
            if (stem_pos != std::string::npos) {
                matches++;
                total_weight += 3;
                if (static_cast<int>(stem.length()) > best_match_length) {
                    best_match_length = stem.length();
                }
            }
        }

        // 2. ВХОЖДЕНИЕ С БОЛЬШОЙ БУКВЫ (для имён собственных)
        if (keyword.length() > 3 && std::isupper(static_cast<unsigned char>(keyword[0]))) {
            std::string keyword_capital = keyword;
            keyword_capital[0] = std::toupper(static_cast<unsigned char>(keyword[0]));
            
            if (chunk.content.find(keyword_capital) != std::string::npos) {
                total_weight += 2;  // Вхождение с заглавной буквы
            }
        }

        // 3. ЧАСТИЧНОЕ СОВПАДЕНИЕ (подстрока от 4 символов)
        if (keyword_lower.length() >= 4) {
            // Ищем подстроки длиной 4-6 символов
            for (size_t sub_len = std::min(size_t(6), keyword_lower.length()); 
                 sub_len >= 4 && sub_len <= keyword_lower.length(); ++sub_len) {
                std::string sub = keyword_lower.substr(0, sub_len);
                if (chunk_lower.find(sub) != std::string::npos) {
                    total_weight += 1;  // Частичное совпадение
                    break;
                }
            }
        }
    }

    // Бонус за длинные совпадения (имена собственные, термины)
    if (best_match_length >= 6) {
        total_weight += 2;
    }
    if (best_match_length >= 10) {
        total_weight += 3;
    }

    // Нормализуем: максимум 3.0 при полном совпадении всех ключевых слов
    float boost = 1.0f + (static_cast<float>(total_weight) / (keywords.size() * 5.0f)) * 2.0f;

    return std::min(boost, 3.0f);  // Ограничиваем максимум 3.0
}

// === RERANKING: Повторная сортировка результатов ===

std::vector<RagChunk> RagManager::rerank_results(const std::string& query,
                                                  const std::vector<RagChunk>& results,
                                                  const std::vector<std::string>& keywords) {
    if (results.empty()) {
        return results;
    }

    // Генерируем эмбеддинг запроса для учёта векторной близости
    std::vector<float> query_embedding = generate_embedding(query);

    // Копируем результаты для сортировки
    std::vector<std::pair<RagChunk, float>> scored_results;
    scored_results.reserve(results.size());

    for (const auto& chunk : results) {
        float boost = keyword_boost_score(chunk, keywords);

        // Векторная близость запроса и чанка (0..1)
        float vector_sim = 0.0f;
        if (!query_embedding.empty() && !chunk.embedding.empty() &&
            chunk.embedding.size() == query_embedding.size()) {
            vector_sim = cosine_similarity(query_embedding, chunk.embedding);
            if (vector_sim < 0.0f) vector_sim = 0.0f;
        }

        // Итоговый скор: буст ключевых слов + векторная близость
        float score = boost + vector_sim * keyword_boost_weight_;
        scored_results.push_back({chunk, score});
    }

    // Сортируем по убыванию score
    std::sort(scored_results.begin(), scored_results.end(),
              [](const auto& a, const auto& b) {
                  return a.second > b.second;
              });

    // Возвращаем отсортированные чанки
    std::vector<RagChunk> reranked;
    reranked.reserve(scored_results.size());

    for (const auto& pair : scored_results) {
        reranked.push_back(pair.first);
    }

    return reranked;
}

// === ГИБРИДНЫЙ ПОИСК: Векторный + полнотекстовый ===

std::vector<RagChunk> RagManager::search_hybrid(const std::string& query, int k,
                                                 const std::string& path_filter) {
    // Шаг 1: Расширяем запрос (транслитерация + окончания)
    std::vector<std::string> expanded_queries = expand_query(query);

    // Шаг 2: Извлекаем ключевые слова для reranking (оригинальные + транслитерированные)
    std::vector<std::string> keywords = extract_keywords(query);

    // Добавляем транслитерированные ключевые слова
    std::vector<std::string> translit_keywords;
    for (const auto& kw : keywords) {
        std::string translit = transliterate_ru_to_en(kw);
        if (translit != kw) {
            translit_keywords.push_back(translit);
            if (!translit.empty()) {
                std::string translit_cap = translit;
                translit_cap[0] = std::toupper(static_cast<unsigned char>(translit_cap[0]));
                translit_keywords.push_back(translit_cap);
            }
        }
    }

    keywords.insert(keywords.end(), translit_keywords.begin(), translit_keywords.end());

    // === ШАГ 3: ПОЛНОТЕКСТОВЫЙ ПОИСК ===
    std::vector<RagChunk> exact_matches;

    // Учитываем морфологию: для каждого ключевого слова проверяем и само слово,
    // и его стем (омарами -> омар), чтобы находить чанки с любой формой слова.
    for (const auto& keyword : keywords) {
        std::string keyword_lower = to_lower_utf8(keyword);
        std::string stem = Stemmer::stem(keyword_lower);
        std::string match_term = (stem.length() >= 3 && stem != keyword_lower)
                                     ? stem : keyword_lower;

        for (const auto& chunk : external_chunks_) {
            std::string chunk_lower = to_lower_utf8(chunk.content);

            bool matched = chunk_lower.find(keyword_lower) != std::string::npos;
            if (!matched && match_term != keyword_lower) {
                matched = chunk_lower.find(match_term) != std::string::npos;
            }

            if (matched) {
                bool already_added = false;
                for (const auto& existing : exact_matches) {
                    if (existing.document_id == chunk.document_id &&
                        existing.chunk_index == chunk.chunk_index) {
                        already_added = true;
                        break;
                    }
                }

                if (!already_added) {
                    exact_matches.push_back(chunk);
                }
            }
        }
    }

    // Шаг 4: Векторный поиск с оригинальным запросом
    auto vector_results = search_external_documents(query, k * 2);

    // === ШАГ 5: ОБЪЕДИНЯЕМ результаты ===
    for (const auto& vector_chunk : vector_results) {
        bool already_exists = false;
        for (const auto& exact : exact_matches) {
            if (exact.document_id == vector_chunk.document_id &&
                exact.chunk_index == vector_chunk.chunk_index) {
                already_exists = true;
                break;
            }
        }

        if (!already_exists) {
            exact_matches.push_back(vector_chunk);
        }
    }

    // Шаг 6: Если есть ключевые слова, делаем reranking
    if (!keywords.empty() && enable_hybrid_search_) {
        exact_matches = rerank_results(query, exact_matches, keywords);
    }

    // Ограничиваем до k результатов
    if (static_cast<int>(exact_matches.size()) > k) {
        exact_matches.resize(k);
    }

    // Apply path filter if specified
    if (!path_filter.empty()) {
        std::vector<RagChunk> filtered;
        filtered.reserve(exact_matches.size());
        for (const auto& chunk : exact_matches) {
            if (chunk.file_path.find(path_filter) != std::string::npos) {
                filtered.push_back(chunk);
            }
        }
        return filtered;
    }

    return exact_matches;
}

} // namespace core
} // namespace llama_gui
