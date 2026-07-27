#include "../include/core/settings.h"
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// =========================================================================
// Сериализация настроек сервера, batch, grammar и output
// =========================================================================

void Settings::serializeServerRuntimeSettings(json& j) const {
    j["server_runtime"] = {
        {"host", server_runtime_settings_.host},
        {"port", server_runtime_settings_.port},
        {"timeout", server_runtime_settings_.timeout},
        {"api_prefix", server_runtime_settings_.api_prefix},
        {"offline", server_runtime_settings_.offline},
        {"threads_http", server_runtime_settings_.threads_http},
        {"static_path", server_runtime_settings_.static_path},
        {"no_webui", server_runtime_settings_.no_webui},
        {"api_keys", server_runtime_settings_.api_keys},
        {"api_key_file", server_runtime_settings_.api_key_file},
        {"ssl_key_file", server_runtime_settings_.ssl_key_file},
        {"ssl_cert_file", server_runtime_settings_.ssl_cert_file},
        {"embeddings_mode", server_runtime_settings_.embeddings_mode},
        {"reranking_mode", server_runtime_settings_.reranking_mode},
        {"metrics_enabled", server_runtime_settings_.metrics_enabled},
        {"log_disabled", server_runtime_settings_.log_disabled},
        {"log_file", server_runtime_settings_.log_file},
        {"log_colors", server_runtime_settings_.log_colors},
        {"log_verbose", server_runtime_settings_.log_verbose},
        {"log_verbosity", server_runtime_settings_.log_verbosity},
        {"log_format", server_runtime_settings_.log_format},
        {"log_prefix", server_runtime_settings_.log_prefix},
        {"log_timestamps", server_runtime_settings_.log_timestamps},
        {"server_binary_path", server_runtime_settings_.server_binary_path}
    };
}

void Settings::serializeBatchSettings(json& j) const {
    j["batch"] = {
        {"batch_size", batch_settings_.batch_size},
        {"ubatch_size", batch_settings_.ubatch_size},
        {"ctx_size", batch_settings_.ctx_size},
        {"ctx_size_draft", batch_settings_.ctx_size_draft},
        {"threads", batch_settings_.threads},
        {"threads_batch", batch_settings_.threads_batch},
        {"cpu_mask", batch_settings_.cpu_mask},
        {"cpu_range", batch_settings_.cpu_range},
        {"cpu_strict", batch_settings_.cpu_strict},
        {"priority", batch_settings_.priority},
        {"poll_level", batch_settings_.poll_level},
        {"cpu_mask_batch", batch_settings_.cpu_mask_batch},
        {"cpu_range_batch", batch_settings_.cpu_range_batch},
        {"cpu_strict_batch", batch_settings_.cpu_strict_batch},
        {"priority_batch", batch_settings_.priority_batch},
        {"poll_batch", batch_settings_.poll_batch},
        {"cont_batching", batch_settings_.cont_batching},
        {"no_perf", batch_settings_.no_perf}
    };
}

void Settings::serializeGrammarSettings(json& j) const {
    j["grammar"] = {
        {"grammar", grammar_settings_.grammar},
        {"grammar_file", grammar_settings_.grammar_file},
        {"json_schema", grammar_settings_.json_schema},
        {"json_schema_file", grammar_settings_.json_schema_file},
        {"chat_template", grammar_settings_.chat_template},
        {"chat_template_file", grammar_settings_.chat_template_file},
        {"chat_template_kwargs", grammar_settings_.chat_template_kwargs},
        {"use_jinja", grammar_settings_.use_jinja},
        {"no_prefill_assistant", grammar_settings_.no_prefill_assistant},
        {"system_prompt_file", grammar_settings_.system_prompt_file},
        {"default_system_prompt", grammar_settings_.default_system_prompt},
        {"reasoning_format", grammar_settings_.get_reasoning_format_string()},
        {"reasoning_budget", grammar_settings_.reasoning_budget}
    };
}

void Settings::serializeOutputSettings(json& j) const {
    j["output"] = {
        {"n_predict", output_settings_.n_predict},
        {"keep", output_settings_.keep},
        {"special_tokens", output_settings_.special_tokens},
        {"spm_infill", output_settings_.spm_infill},
        {"verbose_prompt", output_settings_.verbose_prompt},
        {"escape_sequences", output_settings_.escape_sequences},
        {"tts_use_guide_tokens", output_settings_.tts_use_guide_tokens},
        {"pooling_type", output_settings_.pooling_type}
    };
}

// =========================================================================
// Десериализация настроек сервера, batch, grammar и output
// =========================================================================

void Settings::deserializeServerRuntimeSettings(const json& j) {
    if (j.contains("server_runtime")) {
        auto& s = j["server_runtime"];
        server_runtime_settings_.host = s.value("host", "127.0.0.1");
        server_runtime_settings_.port = s.value("port", 8080);
        server_runtime_settings_.timeout = s.value("timeout", 600);
        server_runtime_settings_.api_prefix = s.value("api_prefix", "");
        server_runtime_settings_.offline = s.value("offline", false);
        server_runtime_settings_.threads_http = s.value("threads_http", -1);
        server_runtime_settings_.static_path = s.value("static_path", "");
        server_runtime_settings_.no_webui = s.value("no_webui", false);
        server_runtime_settings_.api_keys = s.value("api_keys", std::vector<std::string>{});
        server_runtime_settings_.api_key_file = s.value("api_key_file", "");
        server_runtime_settings_.ssl_key_file = s.value("ssl_key_file", "");
        server_runtime_settings_.ssl_cert_file = s.value("ssl_cert_file", "");
        server_runtime_settings_.embeddings_mode = s.value("embeddings_mode", false);
        server_runtime_settings_.reranking_mode = s.value("reranking_mode", false);
        server_runtime_settings_.metrics_enabled = s.value("metrics_enabled", false);
        server_runtime_settings_.log_disabled = s.value("log_disabled", false);
        server_runtime_settings_.log_file = s.value("log_file", "");
        server_runtime_settings_.log_colors = s.value("log_colors", false);
        server_runtime_settings_.log_verbose = s.value("log_verbose", false);
        server_runtime_settings_.log_verbosity = s.value("log_verbosity", 0);
        server_runtime_settings_.log_format = s.value("log_format", "json");
        server_runtime_settings_.log_prefix = s.value("log_prefix", true);
        server_runtime_settings_.log_timestamps = s.value("log_timestamps", true);
        server_runtime_settings_.server_binary_path = s.value("server_binary_path", "llama-server");
    }
}

void Settings::deserializeBatchSettings(const json& j) {
    if (j.contains("batch")) {
        auto& b = j["batch"];
        batch_settings_.batch_size = b.value("batch_size", 2048);
        batch_settings_.ubatch_size = b.value("ubatch_size", 512);
        batch_settings_.ctx_size = b.value("ctx_size", 4096);
        batch_settings_.ctx_size_draft = b.value("ctx_size_draft", 0);
        batch_settings_.threads = b.value("threads", -1);
        batch_settings_.threads_batch = b.value("threads_batch", -1);
        batch_settings_.cpu_mask = b.value("cpu_mask", "");
        batch_settings_.cpu_range = b.value("cpu_range", "");
        batch_settings_.cpu_strict = b.value("cpu_strict", false);
        batch_settings_.priority = b.value("priority", 0);
        batch_settings_.poll_level = b.value("poll_level", 50);
        batch_settings_.cpu_mask_batch = b.value("cpu_mask_batch", "");
        batch_settings_.cpu_range_batch = b.value("cpu_range_batch", "");
        batch_settings_.cpu_strict_batch = b.value("cpu_strict_batch", false);
        batch_settings_.priority_batch = b.value("priority_batch", 0);
        batch_settings_.poll_batch = b.value("poll_batch", 50);
        batch_settings_.cont_batching = b.value("cont_batching", true);
        batch_settings_.no_perf = b.value("no_perf", false);
    }
}

void Settings::deserializeGrammarSettings(const json& j) {
    if (j.contains("grammar")) {
        auto& g = j["grammar"];
        grammar_settings_.grammar = g.value("grammar", "");
        grammar_settings_.grammar_file = g.value("grammar_file", "");
        grammar_settings_.json_schema = g.value("json_schema", "");
        grammar_settings_.json_schema_file = g.value("json_schema_file", "");
        grammar_settings_.chat_template = g.value("chat_template", "");
        grammar_settings_.chat_template_file = g.value("chat_template_file", "");
        grammar_settings_.chat_template_kwargs = g.value("chat_template_kwargs", "");
        grammar_settings_.use_jinja = g.value("use_jinja", false);
        grammar_settings_.no_prefill_assistant = g.value("no_prefill_assistant", false);
        grammar_settings_.system_prompt_file = g.value("system_prompt_file", "");
        grammar_settings_.default_system_prompt = g.value("default_system_prompt", "You are a helpful assistant.");
        grammar_settings_.set_reasoning_format(g.value("reasoning_format", "none"));
        grammar_settings_.reasoning_budget = g.value("reasoning_budget", -1);
    }
}

void Settings::deserializeOutputSettings(const json& j) {
    if (j.contains("output")) {
        auto& o = j["output"];
        output_settings_.n_predict = o.value("n_predict", -1);
        output_settings_.keep = o.value("keep", 0);
        output_settings_.special_tokens = o.value("special_tokens", false);
        output_settings_.spm_infill = o.value("spm_infill", false);
        output_settings_.verbose_prompt = o.value("verbose_prompt", false);
        output_settings_.escape_sequences = o.value("escape_sequences", true);
        output_settings_.tts_use_guide_tokens = o.value("tts_use_guide_tokens", false);
        output_settings_.pooling_type = o.value("pooling_type", "model");
    }
}

} // namespace core
} // namespace llama_gui
