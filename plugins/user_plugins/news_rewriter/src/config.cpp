#include "config.h"

namespace news_rewriter {

Config default_config() {
    Config cfg;
    cfg.sources.push_back(SourceConfig{
        "https://lenta.ru/rss", "rss", SourceExtract{}, true});
    cfg.rewrite = RewriteConfig{};
    cfg.schedule_minutes = 60;
    cfg.sink = SinkConfig{"local_file", Json::object()};
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
    j["rewrite"] = rewrite;

    j["schedule_minutes"] = cfg.schedule_minutes;

    Json sink = Json::object();
    sink["type"] = cfg.sink.type;
    sink["params"] = cfg.sink.params;
    j["sink"] = sink;

    Json network = Json::object();
    network["timeout_seconds"] = cfg.network.timeout_seconds;
    network["user_agent"] = cfg.network.user_agent;
    network["proxy"] = cfg.network.proxy;
    network["extra_headers"] = cfg.network.extra_headers;
    j["network"] = network;

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
    }

    cfg.schedule_minutes =
        static_cast<int>(j.get("schedule_minutes").as_int(cfg.schedule_minutes));

    const Json& sink = j.get("sink");
    if (sink.is_object()) {
        cfg.sink.type = sink.get("type").as_string(cfg.sink.type);
        const Json& params = sink.get("params");
        if (params.is_object()) cfg.sink.params = params;
    }

    const Json& network = j.get("network");
    if (network.is_object()) {
        cfg.network.timeout_seconds = static_cast<int>(
            network.get("timeout_seconds").as_int(cfg.network.timeout_seconds));
        cfg.network.user_agent =
            network.get("user_agent").as_string(cfg.network.user_agent);
        cfg.network.proxy = network.get("proxy").as_string();
        cfg.network.extra_headers = network.get("extra_headers").as_string();
    }

    return cfg;
}

} // namespace news_rewriter
