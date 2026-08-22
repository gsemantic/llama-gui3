#include "config.h"

namespace news_rewriter {

Config default_config() {
    Config cfg;
    cfg.sources.push_back(SourceConfig{
        "https://lenta.ru/rss", "rss", SourceExtract{}, true});
    cfg.rewrite = RewriteConfig{};
    cfg.schedule_minutes = 60;
    cfg.sink.type = "local_file";
    cfg.sink.params = Json::object();
    cfg.network = NetworkConfig{};
    return cfg;
}

namespace {

Json source_to_json(const SourceConfig& s) {
    Json j = Json::object();
    j["url"] = s.url;
    j["type"] = s.type;
    j["enabled"] = s.enabled;
    Json extract = Json::object();
    extract["title_marker"] = s.extract.title_marker;
    extract["body_marker"] = s.extract.body_marker;
    j["extract"] = extract;
    j["preview"] = s.preview;
    return j;
}

SourceConfig source_from_json(const Json& j) {
    SourceConfig s;
    s.url = j.get("url").as_string();
    s.type = j.get("type").as_string("rss");
    s.enabled = j.get("enabled").as_bool(true);
    const Json& e = j.get("extract");
    s.extract.title_marker = e.get("title_marker").as_string();
    s.extract.body_marker = e.get("body_marker").as_string();
    s.preview = j.get("preview").as_bool(false);
    return s;
}

} // namespace

Json config_to_json(const Config& cfg) {
    Json j = Json::object();

    Json sources = Json::array();
    for (const auto& s : cfg.sources) sources.push(source_to_json(s));
    j["sources"] = sources;

    Json rewrite = Json::object();
    rewrite["language"] = cfg.rewrite.language;
    rewrite["tone"] = cfg.rewrite.tone;
    rewrite["prompt_template"] = cfg.rewrite.prompt_template;
    rewrite["max_words"] = cfg.rewrite.max_words;
    rewrite["max_input_chars"] = cfg.rewrite.max_input_chars;
    Json seo = Json::object();
    seo["enabled"] = cfg.rewrite.seo.enabled;
    seo["combine_with_rewrite"] = cfg.rewrite.seo.combine_with_rewrite;
    seo["prompt_template"] = cfg.rewrite.seo.prompt_template;

    Json writing = Json::object();
    writing["max_sentence_words"] = cfg.rewrite.seo.writing.max_sentence_words;
    writing["max_paragraph_words"] = cfg.rewrite.seo.writing.max_paragraph_words;
    writing["min_transition_ratio"] = cfg.rewrite.seo.writing.min_transition_ratio;
    writing["max_passive_ratio"] = cfg.rewrite.seo.writing.max_passive_ratio;
    writing["require_keyphrase_title"] = cfg.rewrite.seo.writing.require_keyphrase_title;
    writing["require_keyphrase_first_paragraph"] =
        cfg.rewrite.seo.writing.require_keyphrase_first_paragraph;
    writing["require_keyphrase_one_heading"] =
        cfg.rewrite.seo.writing.require_keyphrase_one_heading;
    writing["max_words_before_first_heading"] =
        cfg.rewrite.seo.writing.max_words_before_first_heading;
    writing["min_words"] = cfg.rewrite.seo.writing.min_words;
    Json fb = Json::array();
    fb.push(Json(cfg.rewrite.seo.writing.target_flesch_band.first));
    fb.push(Json(cfg.rewrite.seo.writing.target_flesch_band.second));
    writing["target_flesch_band"] = fb;
    Json db = Json::array();
    db.push(Json(cfg.rewrite.seo.writing.keyphrase_density_band.first));
    db.push(Json(cfg.rewrite.seo.writing.keyphrase_density_band.second));
    writing["keyphrase_density_band"] = db;
    writing["max_consecutive_same_start"] =
        cfg.rewrite.seo.writing.max_consecutive_same_start;
    writing["autofix_paragraphs"] = cfg.rewrite.seo.writing.autofix_paragraphs;
    writing["autofix_sentences"] = cfg.rewrite.seo.writing.autofix_sentences;
    writing["autofix_transitions"] = cfg.rewrite.seo.writing.autofix_transitions;
    writing["llm_refine"] = cfg.rewrite.seo.writing.llm_refine;
    writing["read_ease_good"] = cfg.rewrite.seo.writing.read_ease_good;
    writing["read_ease_ok"] = cfg.rewrite.seo.writing.read_ease_ok;
    seo["writing"] = writing;

    Json delivery = Json::object();
    delivery["meta_prefix"] = cfg.rewrite.seo.delivery.meta_prefix;
    delivery["set_wp_title"] = cfg.rewrite.seo.delivery.set_wp_title;
    delivery["optimize_slug"] = cfg.rewrite.seo.delivery.optimize_slug;
    delivery["og_tags"] = cfg.rewrite.seo.delivery.og_tags;
    delivery["twitter_tags"] = cfg.rewrite.seo.delivery.twitter_tags;
    delivery["canonical"] = cfg.rewrite.seo.delivery.canonical;
    seo["delivery"] = delivery;

    Json tax = Json::object();
    tax["enabled"] = cfg.rewrite.taxonomy.enabled;
    tax["auto_assign"] = cfg.rewrite.taxonomy.auto_assign;
    rewrite["taxonomy"] = tax;

    rewrite["seo"] = seo;
    j["rewrite"] = rewrite;

    j["schedule_minutes"] = cfg.schedule_minutes;

    Json sink = Json::object();
    sink["type"] = cfg.sink.type;
    sink["params"] = cfg.sink.params;
    sink["output_dir"] = cfg.sink.output_dir;
    j["sink"] = sink;

    Json network = Json::object();
    network["timeout_seconds"] = cfg.network.timeout_seconds;
    network["user_agent"] = cfg.network.user_agent;
    network["proxy"] = cfg.network.proxy;
    network["extra_headers"] = cfg.network.extra_headers;
    network["headless_enabled"] = cfg.network.headless_enabled;
    network["browser_path"] = cfg.network.browser_path;
    network["headless_timeout_ms"] = cfg.network.headless_timeout_ms;
    j["network"] = network;

    j["max_items_per_source"] = cfg.max_items_per_source;
    j["max_age_hours"] = cfg.max_age_hours;
    j["max_retries"] = cfg.max_retries;

    return j;
}

Config config_from_json(const Json& j) {
    Config cfg = default_config();

    const Json& sources = j.get("sources");
    if (sources.is_array()) {
        cfg.sources.clear();
        for (std::size_t i = 0; i < sources.size(); i++) {
            const Json& item = sources[i];
            if (item.is_object()) cfg.sources.push_back(source_from_json(item));
        }
    }

    const Json& rewrite = j.get("rewrite");
    if (rewrite.is_object()) {
        cfg.rewrite.language = rewrite.get("language").as_string(cfg.rewrite.language);
        cfg.rewrite.tone = rewrite.get("tone").as_string(cfg.rewrite.tone);
        cfg.rewrite.prompt_template =
            rewrite.get("prompt_template").as_string(cfg.rewrite.prompt_template);
        cfg.rewrite.max_words =
            static_cast<int>(rewrite.get("max_words").as_int(cfg.rewrite.max_words));
        cfg.rewrite.max_input_chars =
            static_cast<int>(rewrite.get("max_input_chars").as_int(cfg.rewrite.max_input_chars));
        const Json& seo = rewrite.get("seo");
        if (seo.is_object()) {
            cfg.rewrite.seo.enabled = seo.get("enabled").as_bool(cfg.rewrite.seo.enabled);
            cfg.rewrite.seo.combine_with_rewrite = seo.get("combine_with_rewrite")
                .as_bool(cfg.rewrite.seo.combine_with_rewrite);
            cfg.rewrite.seo.prompt_template =
                seo.get("prompt_template").as_string(cfg.rewrite.seo.prompt_template);
            const Json& w = seo.get("writing");
            if (w.is_object()) {
                auto& wc = cfg.rewrite.seo.writing;
                wc.max_sentence_words = static_cast<int>(
                    w.get("max_sentence_words").as_int(wc.max_sentence_words));
                wc.max_paragraph_words = static_cast<int>(
                    w.get("max_paragraph_words").as_int(wc.max_paragraph_words));
                wc.min_transition_ratio =
                    w.get("min_transition_ratio").as_double(wc.min_transition_ratio);
                wc.max_passive_ratio =
                    w.get("max_passive_ratio").as_double(wc.max_passive_ratio);
                wc.require_keyphrase_title =
                    w.get("require_keyphrase_title").as_bool(wc.require_keyphrase_title);
                wc.require_keyphrase_first_paragraph = w.get("require_keyphrase_first_paragraph")
                    .as_bool(wc.require_keyphrase_first_paragraph);
                wc.require_keyphrase_one_heading = w.get("require_keyphrase_one_heading")
                    .as_bool(wc.require_keyphrase_one_heading);
                wc.max_words_before_first_heading = static_cast<int>(
                    w.get("max_words_before_first_heading")
                        .as_int(wc.max_words_before_first_heading));
                wc.min_words = static_cast<int>(
                    w.get("min_words").as_int(wc.min_words));
                const Json& fb = w.get("target_flesch_band");
                if (fb.is_array() && fb.size() >= 2) {
                    wc.target_flesch_band = {
                        static_cast<int>(fb[0].as_int(wc.target_flesch_band.first)),
                        static_cast<int>(fb[1].as_int(wc.target_flesch_band.second))};
                }
                const Json& db = w.get("keyphrase_density_band");
                if (db.is_array() && db.size() >= 2) {
                    wc.keyphrase_density_band = {
                        db[0].as_double(wc.keyphrase_density_band.first),
                        db[1].as_double(wc.keyphrase_density_band.second)};
                }
                wc.max_consecutive_same_start = static_cast<int>(
                    w.get("max_consecutive_same_start")
                        .as_int(wc.max_consecutive_same_start));
                wc.autofix_paragraphs =
                    w.get("autofix_paragraphs").as_bool(wc.autofix_paragraphs);
                wc.autofix_sentences =
                    w.get("autofix_sentences").as_bool(wc.autofix_sentences);
                wc.autofix_transitions =
                    w.get("autofix_transitions").as_bool(wc.autofix_transitions);
                wc.llm_refine = w.get("llm_refine").as_bool(wc.llm_refine);
                wc.read_ease_good =
                    w.get("read_ease_good").as_double(wc.read_ease_good);
                wc.read_ease_ok = w.get("read_ease_ok").as_double(wc.read_ease_ok);
            }

            const Json& d = seo.get("delivery");
            if (d.is_object()) {
                auto& dc = cfg.rewrite.seo.delivery;
                dc.meta_prefix = d.get("meta_prefix").as_string(dc.meta_prefix);
                dc.set_wp_title = d.get("set_wp_title").as_bool(dc.set_wp_title);
                dc.optimize_slug = d.get("optimize_slug").as_bool(dc.optimize_slug);
                dc.og_tags = d.get("og_tags").as_bool(dc.og_tags);
                dc.twitter_tags = d.get("twitter_tags").as_bool(dc.twitter_tags);
                dc.canonical = d.get("canonical").as_bool(dc.canonical);
            }
        }

        const Json& tax = rewrite.get("taxonomy");
        if (tax.is_object()) {
            cfg.rewrite.taxonomy.enabled =
                tax.get("enabled").as_bool(cfg.rewrite.taxonomy.enabled);
            cfg.rewrite.taxonomy.auto_assign =
                tax.get("auto_assign").as_bool(cfg.rewrite.taxonomy.auto_assign);
        }
    }

    cfg.schedule_minutes =
        static_cast<int>(j.get("schedule_minutes").as_int(cfg.schedule_minutes));

    const Json& sink = j.get("sink");
    if (sink.is_object()) {
        cfg.sink.type = sink.get("type").as_string(cfg.sink.type);
        const Json& params = sink.get("params");
        if (params.is_object()) cfg.sink.params = params;
        cfg.sink.output_dir = sink.get("output_dir").as_string(cfg.sink.output_dir);
    }

    const Json& network = j.get("network");
    if (network.is_object()) {
        cfg.network.timeout_seconds = static_cast<int>(
            network.get("timeout_seconds").as_int(cfg.network.timeout_seconds));
        // Дефолтный User-Agent — браузерный (Яндекс/Chromium), чтобы сайты не
        // блокировали плагин как бота. Старый бот-UA "news_rewriter/1.0"
        // (блокируется VK и др. 302-редиректом) при загрузке автоматически
        // заменяется на дефолт; пользовательский UA сохраняется как есть.
        const std::string ua =
            network.get("user_agent").as_string(cfg.network.user_agent);
        cfg.network.user_agent =
            (ua == "news_rewriter/1.0") ? std::string(kDefaultUserAgent) : ua;
        cfg.network.proxy = network.get("proxy").as_string();
        cfg.network.extra_headers = network.get("extra_headers").as_string();
        cfg.network.headless_enabled =
            network.get("headless_enabled").as_bool(cfg.network.headless_enabled);
        cfg.network.browser_path =
            network.get("browser_path").as_string(cfg.network.browser_path);
        cfg.network.headless_timeout_ms = static_cast<int>(
            network.get("headless_timeout_ms").as_int(cfg.network.headless_timeout_ms));
    }

    cfg.max_items_per_source =
        static_cast<int>(j.get("max_items_per_source").as_int(cfg.max_items_per_source));
    cfg.max_age_hours =
        static_cast<int>(j.get("max_age_hours").as_int(cfg.max_age_hours));
    cfg.max_retries = static_cast<int>(j.get("max_retries").as_int(cfg.max_retries));

    return cfg;
}

} // namespace news_rewriter
