#pragma once

#include <string>
#include <vector>

namespace news_rewriter {

// ============================================================================
// SeoAnalyzer — детерминированный анализатор SEO-копирайт-метрик текста.
//
// Не зависит от LLM и сети: чистая статистика текста. Используется для
// выработки «скоркарда» (как светофор Yoast) и как основа для SeoReformer
// (Phase 2) и LLM-доводки (Phase 3). Базовая линия замеров — Phase 0.
// ============================================================================

enum class SeoStatus { Good, Ok, Poor };

struct SeoMetric {
    std::string key;     // машинное имя (для тестов/UI)
    std::string label;   // человекочитаемая метка
    double value = 0;    // числовое значение
    std::string text;    // значение текстом
    SeoStatus status = SeoStatus::Poor;
};

struct SeoReport {
    std::vector<SeoMetric> metrics;
    int score = 0;                       // 0..100, итог
    std::vector<std::string> issues;     // описания poor-метрик
    std::string summary() const;         // однострочное резюме
};

// Пороги/коэффициенты. Значения по умолчанию — отправная точка (нормы Yoast
// как пример, не догма); позже вынесутся в SeoConfig (Phase 4).
struct SeoCriteria {
    int max_sentence_words = 25;       // длина предложения
    int max_paragraph_words = 120;     // длина абзаца
    double min_transition_ratio = 0.30;// доля предл. с переходными словами
    double max_passive_ratio = 0.10;   // доля пассивных предложений
    bool require_keyphrase_title = true;
    bool require_keyphrase_first_paragraph = true;
    bool require_keyphrase_one_heading = true;
    int max_words_before_first_heading = 300;
    int min_words = 300;
    double keyphrase_density_min = 0.005;
    double keyphrase_density_max = 0.030;
    int max_consecutive_same_start = 3;
    // Flesch (EN): score = a - b*(words/sentences) - c*(syllables/words)
    double flesch_a = 206.835;
    double flesch_b = 1.015;
    double flesch_c = 84.6;
    int flesch_min = 60;
    int flesch_max = 70;
    // RU-удобочитаемость считается по ASL (ср. длина предложения), т.к.
    // классическая формула Flesch для русского даёт отрицательные значения
    // (у RU-слов ~3 слога). Возвращаемый индекс — 0..100, больше = легче.
    double ru_read_ease_good = 70;  // >= -> Good
    double ru_read_ease_ok = 50;    // >= -> Ok, иначе Poor
};

class SeoAnalyzer {
public:
    // Полный анализ тела статьи (+ опц. заголовок, ключевая фраза, язык).
    // lang — "ru"/"en"/...; влияет на слоги/пассив/Flesch.
    static SeoReport analyze(const std::string& body,
                             const std::string& title,
                             const std::string& focus_keyword,
                             const std::string& lang = "ru",
                             const SeoCriteria& crit = SeoCriteria{});

    // --- Низкоуровневые помощники (тестируемые) ---

    // Абзацы: разбиение по одному+ пустых строк.
    static std::vector<std::string> split_paragraphs(const std::string& text);

    // Предложения: учёт сокращений (т.д., г., e.g., ...) и десятичных дробей.
    static std::vector<std::string> split_sentences(const std::string& text);

    // Слова: буквенные (лат/кириллица) и цифровые последовательности.
    static std::vector<std::string> split_words(const std::string& text);

    // Слоги в слове (эвристика: гласные группы). lang = "ru"/"en"/...
    static int count_syllables(const std::string& word, const std::string& lang);

    // Предложение содержит хотя бы одно переходное слово/фразу (по языку).
    static bool has_transition_word(const std::string& sentence,
                                    const std::string& lang);

    // Предложение пассивно (эвристика: быть + страд. причастие / краткое прич.).
    static bool is_passive(const std::string& sentence, const std::string& lang);

    // Индекс удобочитаемости Flesch (по языку/коэффициентам crit).
    static double flesch(const std::string& text, const std::string& lang,
                         const SeoCriteria& crit = SeoCriteria{});

    // Подзаголовки (строки, начинающиеся с "# " — как в body_to_html).
    static std::vector<std::string> extract_headings(const std::string& body);
};

} // namespace news_rewriter
