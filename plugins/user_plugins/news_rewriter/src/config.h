#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace news_rewriter {

// Ключ в настройках хоста, где хранится JSON-конфигурация плагина.
constexpr const char* kConfigKey = "news_rewriter.config";

// Маркеры извлечения для type="page" (пусто = эвристика).
struct SourceExtract {
    std::string title_marker;
    std::string body_marker;
};

struct SourceConfig {
    std::string url;
    std::string type;            // "rss" | "atom" | "page" | "article"
    SourceExtract extract;
    bool enabled = true;
    // Предпросмотр (разведка): для type="page" вместо авто-извлечения и сразу
    // рерайта — сначала предлагает пользователю варианты текста/фото, и только
    // после явного одобрения извлечённый материал идёт в рерайт. Не ломает
    // обычный (без подтверждения) поток обхода.
    bool preview = false;
};

// Копирайт-нормы для механического приведения (SeoReformer, Phase 2) и
// LLM-доводки (Phase 3). Пороги — в конфиге, не хардкод (принцип плана:
// нормы Yoast — отправная точка, не догма, легко переопределяются).
struct SeoWritingConfig {
    int  max_sentence_words        = 25;   // длина предложения
    int  max_paragraph_words       = 120;  // длина абзаца
    double min_transition_ratio    = 0.20; // доля предл. с переходными словами
    double max_passive_ratio       = 0.10; // доля пассивных предложений
    bool require_keyphrase_title           = false; // ключ. фраза в заголовке (выкл: пусть будет во вступлении)
    bool require_keyphrase_first_paragraph = true;  // … в 1-м абзаце
    bool require_keyphrase_one_heading     = true;  // … в подзаголовке
    int  max_words_before_first_heading    = 300;  // текст до 1-го подзаголовка
    int  min_words                  = 300;  // мин. объём статьи
    // Пороги удобочитаемости (RU-индекс 130−4.5·ASL, 0..100): ниже ok_good —
    // Good, ниже ok — Ok, иначе Poor. Ослаблены, т.к. переводные новости часто
    // имеют ~18-словные предложения (см. обсуждение красного SEO-скоркарда).
    double read_ease_good = 70;   // >= -> Good
    double read_ease_ok   = 45;   // >= -> Ok, иначе Poor
    std::pair<int,int> target_flesch_band   = {60, 70};  // Flesch (EN) band
    std::pair<double,double> keyphrase_density_band = {0.005, 0.03};
    int  max_consecutive_same_start = 3;   // повтор стартового слова
    // Механическое приведение (SeoReformer, Phase 2) — детерминировано, без LLM.
    bool autofix_paragraphs  = true;  // дробить длинные абзацы
    bool autofix_sentences   = true;  // дробить длинные предложения
    bool autofix_transitions = false; // вставлять переходные слова (рискованно)
    bool llm_refine          = true; // Phase 3: второй LLM-проход (по умолч. вкл)
};

// Доставка SEO-меты в WordPress (Phase 5) — через namespace-ключи nr_seo_*.
struct SeoDeliveryConfig {
    std::string meta_prefix = "nr_seo"; // nr_seo_title / nr_seo_description / ...
    bool set_wp_title = false;   // true => title==seo_title (H1 и <title> совпадут)
    bool optimize_slug = true;   // slug из focus_keyword (транслит)
    bool og_tags = true;         // OG-теги (mu-plugins/nr-seo.php)
    bool twitter_tags = true;    // Twitter-теги
    bool canonical = true;        // canonical-ссылка
};

// Автоматическая SEO-оптимизация (см. rewriter.cpp, seo_reformer.cpp).
struct SeoConfig {
    bool enabled = false;            // выключено = обратно совместимо (старое поведение)
    // Генерировать SEO в ОДНОМ запросе с рерайтом (общий JSON-ответ) вместо
    // отдельного второго облачного вызова. Экономит квоту/лимиты (актуально
    // при rate-limit), но требует от модели строгого JSON-формата. При сбое
    // разбора плагин откатывается к двум отдельным вызовам.
    bool combine_with_rewrite = true;
    std::string prompt_template =
        "Ты — SEO-редактор. По переписанной новости верни ТОЛЬКО валидный JSON "
        "без пояснений и без markdown-разметки, в точности такого вида:\n"
        "{\"focus_keyword\":\"...\",\"meta_description\":\"...\",\"seo_title\":\"...\"}\n"
        "Правила:\n"
        "- focus_keyword — ключевая фраза из текста, 2-4 слова, строго в нижнем "
        "регистре, на языке статьи;\n"
        "- meta_description — одно предложение, 150-160 символов, без кавычек по "
        "краям, описывает суть новости;\n"
        "- seo_title — до 60 символов, привлекательный и точный (ключевую фразу "
        "НЕ включай — она уходит во вступление статьи, а не в заголовок).\n"
        "Ответ строго JSON.\n\n"
        "Язык: {language}\nЗаголовок: {title}\nТекст: {body}";

    SeoWritingConfig writing;    // копирайт-нормы (Phase 2/3)
    SeoDeliveryConfig delivery;  // доставка мета (Phase 5)
};

// Сохранение ссылок в выходном материале (SEO: внешние — из оригинала,
// внутренние — «похожие материалы» на выходном сайте). См. worker.cpp и
// sink_wordpress.cpp / sink_local_file.cpp.
struct LinkConfig {
    // Сохранять ВСЕ внешние ссылки оригинала в выходной статье (блок
    // «Источники» / «Внешние ссылки»). Эффект есть во всех режимах, но по
    // умолчанию включается для type="article".
    bool preserve_external = false;
    // Искать на выходном сайте (WordPress) похожие материалы по сгенерированным
    // тегам статьи и добавлять 1–2 внутренние ссылки на них (блок «Читайте
    // также»). Если подходящих нет — ничего не добавляем. Работает только для
    // sink типа wordpress (нужен доступ к REST API сайта).
    bool internal_related = false;
    int internal_related_max = 2;   // 1..2 (по SEO — не более двух)
};

// Автоперевод таксономии (рубрики/теги источника → русские названия) и
// проставление их в приёмник (WordPress). См. rewriter.cpp (translate_taxonomy)
// и sink_wordpress.cpp (резолв/создание терминов с соблюдением иерархии).
struct TaxonomyConfig {
    bool enabled = false;        // переводить рубрики/теги из источника на русский
    bool auto_assign = true;     // проставлять в WP (иначе только хранить в article)
};

struct RewriteConfig {
    std::string language = "ru";
    std::string tone = "нейтральный";
    std::string prompt_template =
        "Тебе дана РОВНО ОДНА новость (один материал). Перепиши её своими "
        "словами, сохранив все факты. НЕ делай обзор, сводку или дайджест и НЕ "
        "объединяй с другими материалами. Язык: {language}. Тон: {tone}.\n\n"
        "Правила:\n"
        "- На входе — одна статья, на выходе — тоже РОВНО ОДНА переписанная "
        "статья (число рерайтов всегда равно числу оригиналов).\n"
        "- Сохрани относящиеся к ЭТОЙ новости заголовок, дату и фото: они "
        "передаются отдельно и не входят в текст.\n"
        "- Формат ответа: первая строка — переписанный заголовок, затем пустая "
        "строка, затем переписанный текст новости.\n\n"
        "Заголовок: {title}\nТекст: {body}";
    int max_words = 0;               // 0 = без ограничения (примерный объём статьи)
    int max_input_chars = 12000;     // обрезка текста статьи перед отправкой в LLM
    SeoConfig seo;                   // авто-SEO: ключевое слово / meta-описание (отд. LLM-запрос)
    TaxonomyConfig taxonomy;         // автоперевод рубрик/тегов источника на русский
};

struct SinkConfig {
    std::string type = "local_file";
    Json params = Json::object();
    std::string output_dir;          // пусто = каталог данных приложения
    std::string data_dir;            // каталог данных приложения (runtime, для .env)
};

// Дефолтный User-Agent — реальный браузер (Яндекс.Браузер на движке Chromium),
// чтобы сайты не блокировали плагин как бота. Например, VK и ряд других
// отдают 302/челлендж для нестандартных UA вроде "news_rewriter/1.0"
// (см. config.cpp: миграция старого бот-UA).
constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 YaBrowser/24.7.0.0 "
    "Yowser/2.5 Safari/537.36";

struct NetworkConfig {
    int timeout_seconds = 20;
    // Дефолтный User-Agent (см. kDefaultUserAgent выше).
    std::string user_agent = kDefaultUserAgent;
    std::string proxy;           // пусто = системный прокси
    std::string extra_headers;   // "Header: value\n..." (для авторизации и пр.)

    // Рендеринг страниц через headless-браузер (Chromium). Нужен для сайтов,
    // отдающих контент через JS (SPA, напр. VK.ru): обычный HTTP-фетч получает
    // пустую «оболочку», и статью невозможно извлечь. headless-браузер
    // исполняет JS и возвращает уже отрендеренный DOM.
    bool headless_enabled = false;       // принудительно рендерить ВСЕ page-страницы
    std::string browser_path = "chromium";  // исполняемый файл (путь или имя в PATH)
    int headless_timeout_ms = 30000;     // предельное время рендера (wall-clock)
};

struct Config {
    std::vector<SourceConfig> sources;
    RewriteConfig rewrite;
    int schedule_minutes = 60;   // 0 = только ручной запуск
    SinkConfig sink;
    NetworkConfig network;
    int max_items_per_source = 0;    // 0 = без ограничения (кол-во свежих статей)
    int max_age_hours = 0;           // 0 = без ограничения (свежесть в часах)
    int max_retries = 3;             // повторных попыток после 1-го сбоя источника
    LinkConfig links;                // сохранение внешних/внутренних ссылок в выходе
};

// Чистая сериализация/десериализация (без зависимостей от хоста).
Config default_config();
Json config_to_json(const Config& cfg);
Config config_from_json(const Json& j);

} // namespace news_rewriter
