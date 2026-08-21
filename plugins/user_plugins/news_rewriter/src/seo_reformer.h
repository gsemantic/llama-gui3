#pragma once

#include <string>
#include <vector>

#include "seo_analyzer.h"

namespace news_rewriter {

// ============================================================================
// SeoReformer — Phase 2: детерминированное МЕХАНИЧЕСКОЕ приведение уже
// сгенерированного текста к SEO-копирайт-нормам, БЕЗ LLM и сети.
//
// Гарантирует «жёсткие» правила (длина абзацев/предложений), не расходуя
// квоту/лимиты. For validation используйте SeoAnalyzer::analyze() на
// результате. Не умеет (и не пытается) вставлять ключевую фразу в нужные
// места — это делает LLM-доводка (Phase 3). Механическая вставка переходных
// слов опциональна и ПО УМОЛЧАНИЮ ВЫКЛ (портит стиль).
//
// Текст ожидается в markdown-виде, как в Article::body_rewritten:
//   - абзацы разделены пустой строкой (`\n\n`);
//   - подзаголовки — строки, начинающиеся с `# ` (не дробятся).
// ============================================================================

struct SeoReformConfig {
    int max_paragraph_words = 120;   // дробить абзацы длиннее
    int max_sentence_words  = 25;    // дробить предложения длиннее
    bool autofix_paragraphs  = true; // дробить длинные абзацы по границам предложений
    bool autofix_sentences   = true; // дробить длинные предложения по запятым
    bool autofix_transitions = false;// вставлять переходные слова (рискованно, дефолт выкл)
    std::string lang = "ru";         // язык (для переходных слов и капитализации)
};

struct SeoReformResult {
    std::string reformed_body;        // итоговый (возможно изменённый) текст
    int paragraphs_split = 0;         // сколько абзацев было разбито
    int sentences_split  = 0;         // сколько предложений было разбито
    int transitions_added = 0;        // сколько переходных слов добавлено
    std::vector<std::string> notes;   // что сделано / что осталось вне нормы
};

class SeoReformer {
public:
    // Основной вход: применяет включённые в cfg преобразования к body.
    static SeoReformResult reform(const std::string& body,
                                  const SeoReformConfig& cfg = SeoReformConfig{});

    // --- низкоуровневые помощники (тестируемые) ---

    // Дробит абзацы тела длиннее max_words по границам предложений (ближе к
    // середине). Подзаголовки (`# `) не трогает.
    static std::string split_long_paragraphs(const std::string& body, int max_words);

    // Дробит ОДНО предложение длиннее max_words: заменяет запятую, ближайшую к
    // середине, на точку и капитализирует следующее слово. Рекурсивно докалывает
    // получившиеся части. Если запятых нет — возвращает как есть (помечается
    // скоркардом как poor).
    static std::string split_long_sentence(const std::string& sentence, int max_words,
                                           const std::string& lang = "ru");

    // Опционально: добавляет переходное слово в начало предложения, если своего
    // ещё нет. Для RU — «Кроме того, », для EN — «Moreover, ».
    static std::string add_transition(const std::string& sentence, const std::string& lang);
};

} // namespace news_rewriter
