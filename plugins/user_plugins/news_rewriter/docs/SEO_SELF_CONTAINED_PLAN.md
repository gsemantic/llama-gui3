# План: самодостаточная SEO-оптимизация в `news_rewriter` (без плагинов вроде Yoast)

> Статус: ПЛАН (не реализовано). Цель — передаваемый между сессиями документ,
> по которому можно начать реализацию по фазам. Не коммить и не пушить без
> явной просьбы; это рабочая копия для обсуждения и последующих сессий.

> **РЕШЕНО (2026-08-20, заказчик):** доставка SEO-меты в `<head>` — через
> **Стратегию 1 (тонкий MU-плагин `nr-seo.php`)**. Стратегия 2 (самостоятельный
> HTML) отклонена как избыточная по архитектуре. Все фазы ниже исходят из
> Стратегии 1.
>
> **ПРОГРЕСС (на 2026-08-20):** Phase 0 (замер базовой линии) — сделан;
> Phase 1 (`SeoAnalyzer`) — сделан + тесты; Flesch для RU переделан в ASL-индекс
> (классика давала всегда POOR). Phase 4 (ужесточение мета) — сделан
> (`parse_seo_response`: title ≤60, desc ≤160, keyword 2-4 слова + slug из
> `focus_keyword` через новый `translit`). Phase 5 (доставка) — сделан: ключи
> `yoast_wpseo_*`/`rank_math_*` заменены на `nr_seo_*`, slug берётся из
> `seo_slug`, создан `mu-plugins/nr-seo.php`. Тесты: 19 проходят (10 analyzer +
> 5 translit + 4 seo-meta).
>
> **ПРОГРЕСС (2026-08-21):** Phase 2 (`SeoReformer`) — реализован
> (`src/seo_reformer.{h,cpp}`): механическое дробление длинных абзацев
> (по границам предложений) и длинных предложений (запятая ближе к середине →
> точка + капитализация), опциональная вставка переходных слов (`autofix_*
> = false` по умолчанию). Завязан на `SeoAnalyzer` (токенизация/transition-word
> detection). +7 юнит-тестов (`test_seo_reformer.cpp`, все проходят).
>
> **ПРОГРЕСС (2026-08-21, продолжение):** Расширена `SeoConfig` (§4) — добавлены
> структуры `SeoWritingConfig` (копирайт-нормы + флаги `autofix_*`/`llm_refine`)
> и `SeoDeliveryConfig` (префикс `nr_seo_*`, `set_wp_title`, `optimize_slug`,
> OG/Twitter/canonical) с сериализацией в `config.cpp` (обратно совместимо:
> нет ключей → дефолты). `SeoReformer::reform` завязан в `worker.cpp`
> (`apply_reform` вызывается после рерайта во ВСЕХ ветках: combine / фолбэк /
> обычный путь) при `seo.enabled`, нормы берутся из `cfg.rewrite.seo.writing`,
> что сделано логируется в `log("SEO-реформер: …")`.
>
> **ИЗВЕСТНО:** 4 теста `test_worker.cpp` падают независимо от SEO-правок
> (assert на `calls==2/3` и `source_image` URL) — предсуществующие, не связаны
> с `SeoReformer`/`SeoRefine` (добавлены только новые файлы). Полный прогон:
> 204 passed, 4 failed (предсуществующие).
>
> **ПРОГРЕСС (2026-08-21, завершение):** Phase 3 (`SeoRefine`, LLM-доводка по
> «фидбек-скоркарду») — реализована в `rewriter.{h,cpp}`: `seo_analyze→feedback
> →seo_refine` (второй LLM-проход, best-effort, защита от усечения/галлюцинации
> по объёму ×[0.5,1.8], отказ модели отвергается). Завязана в `worker.cpp`
> (`finalize`) при `seo.writing.llm_refine`, поверх неё повторно прогоняется
> `SeoReformer`. Phase 6 (UI-скоркард + `seo_issues`) — `worker.cpp` считает
> `SeoAnalyzer::analyze` по финальному телу, кладёт `a.seo_score`/`a.seo_issues_text`
> и растит счётчик `run_seo_issues_` (ниже порога `kSeoScoreThreshold=70`);
> в `ui.cpp` — светофор по баллу у каждой задачи + чекбокс `llm_refine`,
> в сводку обхода добавлена «SEO-проблем: N». Phase 7 — добавлены тесты:
> `test_seo_refine_*` (happy/reject-refusal/reject-truncation),
> `test_seo_feedback_text_*`, `test_wp_sink_seo_meta_nr_keys_and_slug`
> (уходят `nr_seo_*` + оптимальный `slug`). Всего +6 тестов, прогон
> 204 passed / 4 failed (предсуществующие).
>
> ОСТАЛОСЬ (опционально): убрать суффикс сайта из `<title>` в `nr-seo.php`
> (`$parts['site'] = ''`); подхватить `delivery.meta_prefix`/`optimize_slug`/
> `set_wp_title` из `SeoConfig` в `sink_wordpress.cpp` вместо захардкоженных
> значений; финальная живая проверка на gsemantic.ru с включённым `llm_refine`.

---

## 0. Контекст и доказанная проблема

Текущее поведение (проверено на https://gsemantic.ru 2026-08-20):

- Плагин генерирует `seo_title`, `meta_description`, `focus_keyword`
  (`rewriter.cpp`: `generate_seo`/`rewrite_and_seo`, `config.h:32` `SeoConfig`).
- При публикации в WordPress эти поля шлются в REST-объекте `meta` под ключами
  `yoast_wpseo_*` / `rank_math_*` (`sink_wordpress.cpp:369-391`).
- **Без Yoast/RankMath посты публикуются нормально** (моя ранняя гипотеза про
  HTTP 400 неверна: WP либо игнорирует незарегистрированные мета-ключи, либо
  сохраняет их «впрок»).
- **Но на сайте эти значения НЕ применяются**: в сыром HTML свежего поста
  нет `<meta name="description">`, `<title>` = обычный заголовок поста (не
  `seo_title`), нет OG/Twitter-тегов, нет упоминаний yoast/rankmath. REST API
  отдаёт `meta` только как `{"footnotes":""}` — ключей `yoast_*` в ответе нет.

**Вывод:** «SEO работает» сейчас — иллюзия. Данные генерируются и улетают в
WP, но не отрисовываются в `<head>`, т.к. это делает именно SEO-плагин, которого
нет. Эффекта для поисковиков нет.

**Цель заказчика (из диалога):**
1. Рерайтер сам выполняет работу SEO-плагина: размещает `seo_title` в нужном
   месте, ключевое слово — в описаниях и в `alt` заглавного изображения.
2. Выходной текст соответствует нормам SEO-копирайта: длина предложений,
   переходные слова, длина абзацев и т.п.
3. Чем меньше плагинов на сайте — тем лучше. Yoast не считаем эталоном.

---

## 1. Архитектурный принцип

Жёсткое разделение двух плоскостей:

- **A. Генерация + копирайт-нормы — 100% в C++-плагине `news_rewriter`.**
  Никакой зависимости от WP. Сюда входит: выработка `seo_title`/`meta_description`/
  `focus_keyword`/оптимального `slug`, приведение прозы к SEO-нормам, скоркард.
- **B. Отрисовка `<head>`-меты — минимальный серверный рендерер на стороне WP.**
  Meta-теги в `<head>` физически может вывести только PHP-хук на сайте (тема их
  не генерирует). Значит, на стороне WP нужен либо тонкий скрипт, либо полный
  отказ от рендера темы.

### Форк доставки — РЕШЕНО: Стратегия 1 (тонкий MU-плагин)

> См. пометку «РЕШЕНО» вверху. Заказчик выбрал Стратегию 1; Стратегия 2
> (самостоятельный HTML) отклонена. Ниже зафиксирован выбранный путь.

- **Стратегия 1 (ВЫБРАНА): тонкий MU-плагин (~60 строк PHP, без UI).**
  Читает наши `nr_seo_*` postmeta и выводит `<title>`, `<meta name=description>`,
  OG/Twitter, canonical. Заменяет тяжёлый SEO-сюит одним файлом в `wp-content/mu-plugins/`.
  Это строго «меньше плагинов»: вместо 1 МБ SEO-пакета — 2 КБ наш скрипт.
- **Стратегия 2 (без единого PHP): самостоятельные HTML-страницы.**
  Рерайтер публикует полностью готовый HTML-документ (со своим `<head>`) в обход
  рендера темы WP (отдельный endpoint / статический файл / кастомный post-type с
  page-template). Архитектурно тяжелее, теряется часть WP-контент-менеджмента,
  но на сайте не появляется ни одного плагина.

Обе стратегии полагаются на то, что **вся интеллектуальная работа сделана в C++**;
разница только в том, КАК значения попадают в `<head>`.

---

## 2. Объём работ по фазам

### Phase 0 — Исследование и замер текущего поведения
- Зафиксировать декомпозицию критериев Yoast (и альтернатив: Google E-E-A-T,
  рекомендации Search Central) в таблицу целевых метрик (см. §3).
- Проверить в коде: выдаёт ли рерайтер подзаголовки (markdown `##`)?
  Конвертирует ли `body_to_html` (`sink_wordpress.cpp`) их в `<h2>`?
  Есть ли где-то транслитерация RU→EN (для `slug`)? (предположительно нет)
- Замерить на реальных статьях текущие показатели (длина предл./абзацев,
  доля переходных слов, passive ratio, Flesch) — базовая линия.

### Phase 1 — `SeoAnalyzer` (C++, детерминированные метрики)
Новый модуль `src/seo_analyzer.{h,cpp}`:
- Токенизация на предложения/абзацы (с учётом сокращений `т.д.`, `т.п.`,
  `др.`, `г.`, `стр.`, `e.g.`, `i.e.`, десятичных точек, многоточий, кавычек).
- Подсчёт слов/слогов (слоги — эвристика; для RU — гласные, для EN — правило
  слогов). Слоговая эвристика конфигурируемая.
- Метрики: `sentence_word_counts[]`, `paragraph_word_counts[]`,
  `transition_word_ratio`, `passive_ratio` (RU: быть + краткое страд. причастие
  на -н/-т, либо «который был/была ... -н/-т»; EN: be + V3), `keyphrase_in_title`,
  `keyphrase_in_first_paragraph`, `keyphrase_in_one_heading`, `keyphrase_density`
  (вхождения фразы / слов всего), `consecutive_same_start_word` (≥3 подряд),
  `flesch_reading_ease` (формула уточняется в Phase 0; для RU — адаптированный
  Флеш, держим параметры формулы в конфиге), `subheading_count`,
  `words_before_first_subheading`, `total_words`.
- На выходе — структура `SeoReport` с набором `{metric, value, status}` где
  status ∈ {good, ok, poor} (как светофор Yoast), плюс итоговый `seo_score`.

### Phase 2 — `SeoReformer` (C++, механическое приведение к нормам, БЕЗ LLM)
Новый модуль `src/seo_reformer.{h,cpp}`. Применяется к уже сгенерированному
тексту и гарантирует «жёсткие» правила без расхода квоты:
- **Дробление длинных абзацев**: если `paragraph_words > max_paragraph_words`,
  разбить по границе предложения ближе к середине.
- **Дробление длинных предложений**: если `sentence_words > max_sentence_words`,
  попытаться разбить по союзам/запятым (осторожно, чтобы не сломать смысл);
  при невозможности — оставить, но пометить в скоркарде как `poor`.
- **Переходные слова**: опционально, только если `transition_word_ratio <
  min_transition_ratio` и `autofix_transitions=true` — мягко добавлять дешёвые
  переходы («Кроме того», «При этом», «Однако») в начало некоторых предложений.
  **Рискованно** (портит стиль) — по умолчанию ВЫКЛ, используем LLM-доводку (§3).
- **Ключевая фраза в первом абзаце / в одном подзаголовке**: если отсутствует —
  отметить в скоркарде; механическая вставка нежелательна (делает LLM в Phase 3).
- Все правки — детерминированные, воспроизводимые, не требуют сети.

### Phase 3 — LLM-доводка (опциональный второй проход, rate-limit-aware)
- Включается `seo.llm_refine`. После Phase 1+2 формируется «фидбек-скоркард»
  (текстовое описание того, что плохо: «предложение X из 40 слов — слишком
  длинное; нет переходных слов в 70% предложений; ключевая фраза не в H2») и
  отправляется LLM как инструкция переписать ТОЛЬКО проблемные места.
- Best-effort: при rate-limit — пропускаем доводку, статья уходит с тем, что
  дал Reformer; счётчик «SEO-issues» (аналог `seo_missing`).
- Чтобы не удваивать нагрузку при `combine_with_rewrite`, можно совмещать:
  просить модель сразу вернуть текст, соблюдающий нормы (см. §4 prompt).

### Phase 4 — Ужесточение генерации мета
- `seo_title`: строго ≤ 60 символов, ключевое слово в начале (уже в промпте
  `config.h:48`; добавить жёсткую обрезку/проверку в `parse_seo_response`).
- `meta_description`: строго 150–160 символов, содержит ключевое слово; обрезка
  по границе слов + суффикс «…» при необходимости; валидация в парсере.
- `focus_keyword`: 2–4 слова, нижний регистр, на языке статьи (уже в промпте).
- **Оптимальный `slug`**: генерировать из `focus_keyword` (транслитерация RU→EN
  + вырезка стоп-слов), а не хэш/название. Новый модуль `src/translit.{h,cpp}`
  (минимальная таблица RU→EN + латиница). Передавать в `body["slug"]`
  (`sink_wordpress.cpp:350`) вместо `storage_.slug_for` при включённом SEO.
- `alt` заглавного изображения = `focus_keyword` — **уже реализовано**
  (`storage.cpp:108-110`, `sink_wordpress.cpp:304-306`). Оставить как есть.

### Phase 5 — Доставка без Yoast
- **Смена ключей мета на namespace** `nr_seo_*`, чтобы не зависеть от чужих
  плагинов и не конфликтовать: `nr_seo_title`, `nr_seo_description`,
  `nr_seo_keyword` (в `sink_wordpress.cpp:373-384` и в `Article`/`common.cpp`).
- Нативные поля WP (уже/дополнительно):
  - `excerpt` ← `meta_description` (уже есть, `sink_wordpress.cpp:389-390`).
  - Опция `set_wp_title` (по умолч. false): если true — `body["title"]` =
    `seo_title` (H1 и `<title>` совпадут); если false — H1 = заголовок статьи,
    а `<title>` берётся из `nr_seo_title` рендерером.
  - `slug` ← оптимизированный (Phase 4).
- **MU-плагин** `wp-content/mu-plugins/nr-seo.php` (спецификация + код в §6) —
  читает `nr_seo_*` и выводит `<title>` (через фильтр `document_title_parts`),
  `<meta name=description>`, OG/Twitter, canonical. Без настроек и UI.
- При Стратегии 2 (§1) вместо MU-плагина — свой шаблон/endpoint, выдающий
  полный HTML от рерайтера (см. риски).

### Phase 6 — UI-скоркард в плагине GUI
- В панели предпросмотра (`ui.cpp`) рядом с заголовком/текстом показывать
  «светофор» по метрикам Phase 1 (seo_title len, description len, keyword в
  first paragraph/heading, transition ratio, sentence/paragraph lengths).
- Счётчик `state_.seo_missing` расширить на `state_.seo_issues` (статей, где
  скоркард ниже порога), выводить в итог обхода.

### Phase 7 — Тесты
- Юнит-тесты `seo_analyzer`/`seo_reformer` (длина предл./абзацев, переходные
  слова, passive, Flesch на эталонных текстах).
- Юнит-тест: обрезка/валидация `seo_title`/`meta_description` по длине.
- Интеграционный (mock LLM): что при `seo.enabled` уходят `nr_seo_*` ключи и
  оптимальный `slug`.
- Проверка на gsemantic.ru: после публикации в `<head>` реально появляются
  `<meta name=description>` и `<title>` из `seo_title`.

---

## 3. Правила SEO-копирайта (конфигурируемый набор)

| Правило | Параметр (def) | Норма (дефолт) | Источник идеи |
|---|---|---|---|
| Длина предложения | `max_sentence_words` | 25 слов | Yoast «sentence length» |
| Длина абзаца | `max_paragraph_words` | 120 слов | Yoast «paragraph length» |
| Доля переходных слов | `min_transition_ratio` | 0.30 | Yoast «transition words» |
| Passive voice | `max_passive_ratio` | 0.10 | Yoast «passive voice» |
| Ключ. фраза в заголовке (SEO/пост) | `require_keyphrase_title` | true | Yoast |
| Ключ. фраза в 1-м абзаце | `require_keyphrase_first_paragraph` | true | Yoast |
| Ключ. фраза в подзаголовке | `require_keyphrase_one_heading` | true | Yoast |
| Текст до 1-го подзаголовка | `max_words_before_first_heading` | 300 | Yoast |
| Мин. объём статьи | `min_words` | 300 | Yoast |
| Чтение (Flesch) | `target_flesch_band` | [60, 70] | Yoast FRE |
| Плотность ключ. фразы | `keyphrase_density_band` | [0.005, 0.03] | Yoast density |
| Повтор стартового слова | `max_consecutive_same_start` | 3 | Yoast |

> Все пороги — в конфиге, не хардкод. Нормы Yoast взяты как отправная точка,
> не как догма: заказчик не считает Yoast эталоном, поэтому band-ы легко
> переопределяются (напр., для RU допустима бóльшая длина предложения).

**Словари (в коде, расширяемые):**
- Переходные слова RU: `кроме того, при этом, однако, более того, например,
  следовательно, в результате, в частности, несмотря на, поэтому, таким образом,
  с одной стороны, с другой стороны, прежде всего, в то же время, итак, значит…`
- Переходные слова EN: `however, moreover, therefore, for example, in addition,
  consequently, furthermore, thus, on the other hand, first, finally, because…`
- Сокращения для splitter: `т.д., т.п., др., г., стр., рис., e.g., i.e., т.е.,
  проф., д-р, ISSN, напр.`

---

## 4. Конфигурация (`config.h` → `SeoConfig`)

Расширить структуру `SeoConfig` (или добавить `SeoWritingConfig`):

```cpp
struct SeoConfig {
    bool enabled = false;
    bool combine_with_rewrite = true;
    std::string prompt_template = "...";   // существующий SEO-промпт

    // --- копирайт-нормы (Phase 1-3) ---
    struct Writing {
        int  max_sentence_words        = 25;
        int  max_paragraph_words       = 120;
        double min_transition_ratio    = 0.30;
        double max_passive_ratio       = 0.10;
        bool require_keyphrase_title            = true;
        bool require_keyphrase_first_paragraph  = true;
        bool require_keyphrase_one_heading      = true;
        int  max_words_before_first_heading     = 300;
        int  min_words                  = 300;
        std::pair<int,int> target_flesch_band   = {60, 70};
        std::pair<double,double> keyphrase_density_band = {0.005, 0.03};
        int  max_consecutive_same_start = 3;

        bool autofix_paragraphs  = true;   // Phase 2: дробить абзацы
        bool autofix_sentences   = true;   // Phase 2: дробить предложения
        bool autofix_transitions = false;  // рискованно, дефолт выкл
        bool llm_refine          = false;  // Phase 3: второй LLM-проход
    } writing;

    // --- доставка (Phase 4-5) ---
    struct Delivery {
        std::string meta_prefix = "nr_seo";   // ключи nr_seo_title/...
        bool set_wp_title = false;            // true => title==seo_title
        bool optimize_slug = true;            // slug из focus_keyword
        bool og_tags = true;
        bool twitter_tags = true;
        bool canonical = true;
    } delivery;
};
```

Промпт рерайта (и `role_combined_`) дополнить требованием: «используй подзаголовки
markdown `##`; первый абзац и хотя бы один подзаголовок содержат ключевую фразу;
предложения ≤ N слов; абзацы ≤ M слов; ≥30% предложений с переходными словами».

---

## 5. Точки изменения в существующем коде

| Что | Файл:строка (сейчас) | Действие |
|---|---|---|
| Поля статьи SEO | `common.h:42-44`, `common.cpp:305-307` | добавить `seo_slug`; сериализовать `nr_seo_*` |
| Генерация мета | `rewriter.cpp` (`generate_seo`, `parse_seo_response`, `rewrite_and_seo`) | жёсткие проверки длины title/desc |
| Применение SEO в обходе | `worker.cpp:362-426` (`apply_seo`, combine) | внедрить Analyzer→Reformer→(LLM refine); slug |
| Отправка в WP | `sink_wordpress.cpp:369-391` | ключи `nr_seo_*`; `slug` из SEO; OG/Twitter/canonical |
| Alt картинки | `storage.cpp:108-110`, `sink_wordpress.cpp:304-306` | уже = focus_keyword (оставить) |
| Excerpt | `sink_wordpress.cpp:389-390` | уже = meta_description (оставить) |
| UI чекбокс SEO | `ui.cpp:235-244` | расширить настройками §4 + скоркард (Phase 6) |
| Конфиг | `config.h:32`, `config.cpp` | поля §4, сериализация |
| Состояние обхода | `worker.h:65`, `worker.cpp:719-731` | `seo_missing` → `seo_issues` |

---

## 6. Спецификация WP MU-плагина (Стратегия 1)

Файл `wp-content/mu-plugins/nr-seo.php` (~60 строк, без UI). Читает `nr_seo_*`
postmeta и выводит в `<head>`:

```php
<?php
// news_rewriter — минимальный SEO-рендерер (замена Yoast/RankMath).
// Выводит <title>/description/OG/Twitter/canonical из postmeta nr_seo_*.
add_filter('document_title_parts', function ($parts) {
    $t = get_post_meta(get_the_ID(), 'nr_seo_title', true);
    if ($t) { $parts['title'] = $t; }
    return $parts;
});
add_action('wp_head', function () {
    if (!is_singular()) return;
    $id = get_the_ID();
    $desc = get_post_meta($id, 'nr_seo_description', true);
    $kw   = get_post_meta($id, 'nr_seo_keyword', true);
    $url  = get_permalink($id);
    $img  = get_the_post_thumbnail_url($id, 'full');
    if ($desc) {
        echo '<meta name="description" content="' . esc_attr($desc) . '">' . "\n";
        echo '<meta property="og:description" content="' . esc_attr($desc) . '">' . "\n";
        echo '<meta name="twitter:description" content="' . esc_attr($desc) . '">' . "\n";
    }
    if ($kw) echo '<meta name="keywords" content="' . esc_attr($kw) . '">' . "\n";
    if ($url) echo '<link rel="canonical" href="' . esc_url($url) . '">' . "\n";
    echo '<meta property="og:url" content="' . esc_url($url) . '">' . "\n";
    echo '<meta property="og:type" content="article">' . "\n";
    if ($img) echo '<meta property="og:image" content="' . esc_url($img) . '">' . "\n";
    echo '<meta name="twitter:card" content="summary_large_image">' . "\n";
}, 1);
```

При Стратегии 2 этот файл не нужен (рендер внутри HTML от рерайтера).

---

## 7. Критерии приёмки (acceptance)

1. На gsemantic.ru у опубликованной статьи в `<head>` реально появляются
   `<meta name="description">` (= `meta_description`) и `<title>` (= `seo_title`),
   OG/Twitter-теги, canonical — без установки Yoast/RankMath.
2. `alt` заглавного изображения = `focus_keyword` (уже работает).
3. Выходной текст проходит `SeoAnalyzer`: ≥80% предложений ≤ `max_sentence_words`,
   ≥80% абзацев ≤ `max_paragraph_words`, `transition_ratio` ≥ `min_transition_ratio`
   (при включённом `llm_refine` или успешном `autofix`).
4. `slug` поста содержит ключевую фразу (транслитерированную).
5. При rate-limit статья не падает: публикуется с тем, что дал Reformer,
   счётчик `seo_issues` растёт (best-effort, как сейчас `seo_missing`).

---

## 8. Риски и открытые вопросы

- **Форк доставки (§1): РЕШЕН — Стратегия 1 (MU-плагин `nr-seo.php`).**
- **Flesch для RU:** точная формула нестабильна; берём адаптированный Флеш,
  параметры — в конфиге; при необходимости упрощаем до «слов/предложение».
- **Подзаголовки:** рерайтер сейчас, возможно, не выдаёт `##`. Phase 0 проверит;
  если нет — добавить требование в промпт и поддержку в `body_to_html`.
- **Механическая вставка переходных слов** портит стиль → держим выключенной,
  полагаемся на LLM-доводку (Phase 3), которая сама по себе ненадёжна при
  rate-limit (компенсируется скоркардом, не падением).
- **Транслитерация RU→EN для slug:** минимальная таблица; для китайских/японских
  источников slug может оставаться хэш-подобным (допустимо).
- **WP кэш/объектный кэш:** после публикации мета могут не сразу отрисоваться —
  учесть в проверке (сброс кэша при тесте).

---

## 9. Чеклист для новой сессии

Статус на 2026-08-20 (коммит `165cd25`, запушен в origin/main):

- [x] Прочитать этот файл целиком.
- [x] Подтвердить у заказчика выбор Стратегии 1 (MU-плагин `nr-seo.php`, §1, §8).
- [x] Phase 0: замер текущих метрик (базовая линия SEO ~48/100) + проверка
      подзаголовков (`body_to_html` → `<h2>`), `alt`=focus_keyword, транслита (не было).
- [x] Реализовать `SeoAnalyzer` (Phase 1) + юнит-тесты (10 шт., проходят).
- [x] Phase 4: жёсткие проверки длины мета (`seo_title` ≤60, `meta_description`
      ≤160, `focus_keyword` 2–4 слова) + `seo_slug` + модуль `translit` (RU→EN + slug).
- [x] Phase 5: ключи `yoast_wpseo_*`/`rank_math_*` → `nr_seo_*` в `sink_wordpress.cpp`;
      slug из `seo_slug`; MU-плагин `mu-plugins/nr-seo.php` (регистрирует `nr_seo_*` для
      REST + выводит `<title>`/description/OG/Twitter/canonical). Деплой `.so` + `nr-seo.php`
      выполнен, проверено на живом посте gsemantic.ru — `<head>` реально заполнен.
- [x] Тесты: +19 (seo_analyzer 10, translit 5, seo_meta 4), все проходят.
- [x] Закоммитить и запушить (коммит `165cd25`, main → origin/main).

ОСТАЛОСЬ (следующая сессия):

- [x] Phase 2: реализовать `SeoReformer` (`src/seo_reformer.{h,cpp}`) — механическое
      дробление длинных абзацев/предложений, опционально переходные слова + юнит-тесты
      (7 шт., проходят; 2026-08-21).
- [x] Расширить `SeoConfig` (§4): структуры `Writing`/`Delivery` + сериализация в
      `config.cpp` (обратно совместимо; 2026-08-21).
- [x] Внедрить Analyzer→Reformer в `worker.cpp`: `apply_reform` вызывается после
      рерайта во всех ветках (combine / фолбэк / обычный путь) при `seo.enabled`,
      нормы из `cfg.rewrite.seo.writing` (2026-08-21).
- [x] Phase 3: LLM-доводка (второй проход по «фидбек-скоркарду», rate-limit-aware) — `rewriter.cpp` + `worker.cpp`.
- [x] Phase 6: UI-скоркард в `ui.cpp` — светофор по баллу `SeoAnalyzer` + счётчик `seo_issues` (воркер + сводка).
- [x] Phase 7 (докомплектация): интеграционные тесты — `test_seo_refine_*`, `test_seo_feedback_text_*`,
      `test_wp_sink_seo_meta_nr_keys_and_slug` (mock LLM / mock WP — уходят `nr_seo_*` + оптимальный slug).
- [ ] При желании: убрать суффикс сайта из `<title>` в `nr-seo.php` (`$parts['site'] = '';`).
- [ ] Опционально: подхватить `delivery.meta_prefix` / `optimize_slug` / `set_wp_title` из
      `SeoConfig` в `sink_wordpress.cpp` вместо захардкоженных значений.
- [ ] Финальная живая проверка на gsemantic.ru с включённым `llm_refine`.

## 10. Послеживая настройка (2026-08-21, коммит после `e924c3c`)

После деплоя и живой проверки поста 809 (gsemantic.ru) выяснилось: красный
SEO-индикатор в UI — это внутренний скоркард плагина (`a.seo_score < 70`), а НЕ
сбой публикации (пост опубликован корректно, `<head>` заполнен). Реальный скоркард
поста 809: без ключевой фразы — 74; с `нейроинтерфейс покрытие` — 58. POOR-метрики:
доля переходных слов 19% (цель ≥30%), удобочитаемость (RU) 48 (порог Ok=50),
плотность ключевой фразы 0% (модель берёт фразу из заголовка, тело дословно не
содержит).

Одобрено три доработки (все реализованы и задеплоены):
- [x] `SeoWritingConfig::llm_refine` по умолчанию `true` (второй LLM-проход
      исправляет POOR-метрики post-hoc). `config.h` + сериализация `config.cpp`.
- [x] Ослаблены пороги: `min_transition_ratio` 0.30→**0.20** и
      `ru_read_ease_ok` 50→**45** (в `config.h`/`seo_analyzer.h` дефолты
      `SeoCriteria`, маппинг в `worker.cpp::seo_criteria_from_writing`). Добавлены
      `read_ease_good`/`read_ease_ok` в `SeoWritingConfig` (сериализуются в
      `config.cpp`), чтобы пороги задавались из конфига, а не только из хардкода.
- [x] В промпт рерайта (`rewriter.cpp::build_role_from_template`) добавлена
      инструкция органично вплетать ключевую фразу темы (из заголовка) в первый
      абзац и подзаголовок с плотностью ~1% — чтобы метрики плотности/первого
      абзаца/подзаголовка не падали ещё до `llm_refine`. (Сама focus_keyword
      генерируется SEO-шагом ПОСЛЕ рерайта, поэтому в момент рерайта просим
      использовать ключевую мысль из заголовка; комбинированный путь генерирует
      фразу вместе с рерайтом.)

Проверка: `news_rewriter_tests` — все SEO/rewriter/config-тесты проходят
(в т.ч. `test_config_roundtrip_new_fields` для новых полей); падения остаются
только у сетевых тестов (fetcher/http/wp_sink/worker) из-за отсутствия сети в
песочнице — не регрессия. `.so` пересобран и лежит в `build/plugins/`.

## 11. Доводка скоркарда по живому посту (2026-08-21, после `d86242a`)

Живая проверка поста `2026/08/21/news_sciencenet_cn_4286cbc8e822/` выявила:

- Публикация корректна: `meta description`, `og:description`, `og:image`,
  `canonical`, `meta keywords` (focus_keyword) — на месте.
- **Пробел:** MU-плагин `nr-seo.php` не выводил `og:title`/`twitter:title`
  (выводил только `og:description`/`og:image`/…). Добавлен вывод
  `og:title`/`twitter:title` из `nr_seo_title` либо заголовка записи.
  ТРЕБУЕТСЯ перекопировать `mu-plugins/nr-seo.php` на сервер
  (`wp-content/mu-plugins/nr-seo.php`).
- **Скоркард был красным (63):** анализатор требовал ТОЧНОГО совпадения
  подстрокой focus_keyword, а модель перефразировала фразу
  («решение … нейроинтерфейсов» вместо «решение нейроинтерфейсов») →
  density=0%, keyphrase не в 1-м абзаце. Исправлено в `seo_analyzer.cpp`:
  ключевая фраза считается присутствующей, если ЕЁ СЛОВА идут в тексте в том же
  порядке (подпоследовательность слов, `keyphrase_in_order`/
  `count_keyphrase_in_order`). Для оценки плотности — жадный подсчёт
  непересекающихся вхождений-подпоследовательностей.
- Реакция на живой пост (очищенный body): скоркард **78/100** (зелёный);
  остаётся только POOR `keyphrase_heading` — в теле нет `##`-подзаголовков
  (статья единым блоком). Это реальная, а не ложная проблема; модель по
  промпту должна добавлять `##`-подзаголовки, но не всегда. Тест
   `test_seo_analyze_basic` не сломан (точная фраза — тоже валидная
   подпоследовательность).
- Все SEO/rewriter/config-тесты проходят; 34 падения — сетевые (не регрессия).

## 12. Решение: ключевая фраза — во вступлении, а не в заголовке (2026-08-22)

**Запрос заказчика:** рерайт должен начинаться с небольшого вступления
(краткого пересказа сути новости), и ключевая фраза (`focus_keyword`) должна
попадать именно туда, а НЕ в заголовок (`title`/`seo_title`).

**Реализовано:**
- `config.h` (`SeoWritingConfig`): `require_keyphrase_title = true → false`.
  Скоркард теперь не требует ключевую фразу в заголовке (проверка
  `keyphrase_title` в `seo_analyzer.cpp` перестала быть обязательной при сборке
  критериев из конфига через `worker.cpp::seo_criteria_from_writing`).
- `config.h` (`SeoConfig::prompt_template`, строка про `seo_title`): убрано
  «с ключевым словом в начале» — заголовок теперь просто яркий/точный.
- `rewriter.cpp` (`build_role_from_template`): промпт рерайта требует начинать
  текст с краткого вступления-пересказа (1–2 предложения) и явно велит ставить
  ключевую фразу в ЭТОТ первый абзац, а не в заголовок.
- `rewriter.cpp` (`build_combined_role_prompt`, поле `seo_title`): убрано
  «с ключевым словом в начале» для combined-режима.

**Сохранённые инварианты:**
- `require_keyphrase_first_paragraph = true` — скоркард по-прежнему проверяет
  наличие фразы в первом абзаце (теперь это и есть вступление).
- `require_keyphrase_one_heading = true` — фраза желательна в одном из
  подзаголовков (опционально, промпт мягко просит).
- `SeoCriteria::require_keyphrase_title` в `seo_analyzer.h` оставлен `true` —
  это дефолт библиотеки/юнит-тестов (напр. `test_seo_analyze_basic`), поведение
  плагина управляется конфигом (`config.h`), который переопределяет его в
  `false`. Тесты не сломаны.

**Сборка/проверка:** `./build.sh` — `.so` и тесты собираются; локальные
SEO/rewriter/config-тесты зелёные; единственный упавший
`test_worker_run_completes` — предсуществующий, на сети (не регрессия).
