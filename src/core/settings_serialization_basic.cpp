#include "../include/core/settings.h"
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// =========================================================================
// Сериализация основных настроек (Display, Server, Chat, Files)
// =========================================================================

void Settings::serializeDisplaySettings(json& j) const {
    j["display"] = {
        {"width", display_settings_.window_width},
        {"height", display_settings_.window_height},
        {"screen_width", display_settings_.screen_width},
        {"screen_height", display_settings_.screen_height},
        {"window_x", display_settings_.window_x},
        {"window_y", display_settings_.window_y},
        {"window_maximized", display_settings_.window_maximized},
        {"auto_resize", display_settings_.auto_resize},
        {"use_dark_theme", display_settings_.use_dark_theme},
        {"font_size", display_settings_.font_size},
        {"font_family", display_settings_.font_family},
        {"enable_animation", display_settings_.enable_animation},
        {"frame_rate_limit", display_settings_.frame_rate_limit},
        {"center_window", display_settings_.center_window},
        {"min_window_width", display_settings_.min_window_width},
        {"min_window_height", display_settings_.min_window_height},
        {"dpi_scale", display_settings_.dpi_scale},
        {"margin", display_settings_.margin},
        {"language", display_settings_.language},
        {"theme", static_cast<int>(current_theme_)}
    };
}

void Settings::serializeServerSettings(json& j) const {
    j["server"] = {
        {"host", server_settings_.host},
        {"port", server_settings_.port},
        {"api_url", server_settings_.api_url},
        {"connection_timeout", server_settings_.connection_timeout},
        {"request_timeout", server_settings_.request_timeout},
        {"max_retries", server_settings_.max_retries},
        {"verify_ssl", server_settings_.verify_ssl}
    };
}

void Settings::serializeChatSettings(json& j) const {
    j["chat"] = {
        {"max_tokens", chat_settings_.max_tokens},
        {"temperature", chat_settings_.temperature},
        {"top_p", chat_settings_.top_p},
        {"top_k", chat_settings_.top_k},
        {"min_p", chat_settings_.min_p},
        {"repeat_penalty", chat_settings_.repeat_penalty},
        {"presence_penalty", chat_settings_.presence_penalty},
        {"frequency_penalty", chat_settings_.frequency_penalty},
        {"mirostat_mode", chat_settings_.mirostat_mode},
        {"mirostat_tau", chat_settings_.mirostat_tau},
        {"mirostat_eta", chat_settings_.mirostat_eta},
        {"stop_on_newline", chat_settings_.stop_on_newline},
        {"default_system_prompt", chat_settings_.default_system_prompt},
        {"threads", chat_settings_.threads},
        {"n_ctx", chat_settings_.n_ctx},
        {"seed", chat_settings_.seed},
        {"tfs_z", chat_settings_.tfs_z},
        {"typical_p", chat_settings_.typical_p},
        {"n_gpu_layers", chat_settings_.n_gpu_layers},
        {"tensor_split", chat_settings_.tensor_split},
        {"numa", chat_settings_.numa},
        {"lora_adapters", chat_settings_.lora_adapters},
        {"lora_base", chat_settings_.lora_base},
        {"mmproj", chat_settings_.mmproj},
        {"grammar", chat_settings_.grammar},
        {"chat_template", chat_settings_.chat_template},
        {"embedding", chat_settings_.embedding},
        {"reverse_prompt", chat_settings_.reverse_prompt},
        {"log_format", chat_settings_.log_format},
        {"verbosity", chat_settings_.verbosity}
    };
}

void Settings::serializeFileSettings(json& j) const {
    j["files"] = {
        {"default_save_path", file_settings_.default_save_path},
        {"auto_save_path", file_settings_.auto_save_path},
        {"auto_save_enabled", file_settings_.auto_save_enabled},
        {"auto_save_interval", file_settings_.auto_save_interval}
    };
}

void Settings::serializePerformanceSettings(json& j) const {
    j["performance"] = {
        {"enable_vsync", performance_settings_.enable_vsync},
        {"target_fps", performance_settings_.target_fps},
        {"idle_fps", performance_settings_.idle_fps},
        {"idle_timeout_ms", performance_settings_.idle_timeout_ms},
        {"enable_smart_redraw", performance_settings_.enable_smart_redraw},
        {"show_performance_overlay", performance_settings_.show_performance_overlay},
        {"performance_update_interval_ms", performance_settings_.performance_update_interval_ms},
        {"enable_logging", performance_settings_.enable_logging},
        {"log_level", performance_settings_.log_level},
        {"log_to_file", performance_settings_.log_to_file},
        {"log_file_path", performance_settings_.log_file_path},
        {"log_flush_policy", performance_settings_.log_flush_policy},
        {"debug_mode", performance_settings_.debug_mode}
    };
}

void Settings::serializeRagSettings(json& j) const {
    j["rag"] = {
        {"embedding_model_path", rag_settings_.embedding_model_path},
        {"embedding_server_url", rag_settings_.embedding_server_url},
        {"max_chunks_in_memory", rag_settings_.max_chunks_in_memory},
        {"similarity_threshold", rag_settings_.similarity_threshold},
        {"max_embedding_cache_size", rag_settings_.max_embedding_cache_size},
        {"embedding_dimension", rag_settings_.embedding_dimension},
        {"max_sequence_length", rag_settings_.max_sequence_length},
        {"max_tokens_per_chunk", rag_settings_.max_tokens_per_chunk},
        {"search_k", rag_settings_.search_k},
        {"mmr_lambda", rag_settings_.mmr_lambda},
        {"enable_mmr", rag_settings_.enable_mmr},
        {"enable_rag", rag_settings_.enable_rag},
        {"enable_caching", rag_settings_.enable_caching},
        {"rag_mode", static_cast<int>(rag_settings_.rag_mode)},

        // Параметры гибридного поиска
        {"enable_hybrid_search", rag_settings_.enable_hybrid_search},
        {"keyword_boost_weight", rag_settings_.keyword_boost_weight},
        {"enable_query_expansion", rag_settings_.enable_query_expansion},

        // Настройки глубокого анализа документа
        {"deep_analysis_mode", static_cast<int>(rag_settings_.deep_analysis.mode)},
        {"deep_analysis_chunks_per_batch", rag_settings_.deep_analysis.chunks_per_batch},
        {"deep_analysis_max_iterations", rag_settings_.deep_analysis.max_iterations},
        {"deep_analysis_enable_progressive_summary", rag_settings_.deep_analysis.enable_progressive_summary},
        {"deep_analysis_final_synthesis_chunks", rag_settings_.deep_analysis.final_synthesis_chunks},
        {"deep_analysis_auto_adjust_context_size", rag_settings_.deep_analysis.auto_adjust_context_size},
        {"deep_analysis_target_context_size", rag_settings_.deep_analysis.target_context_size}
    };
}

// =========================================================================
// Десериализация основных настроек
// =========================================================================

void Settings::deserializeDisplaySettings(const json& j) {
    if (j.contains("display")) {
        auto& d = j["display"];
        display_settings_.window_width = d.value("width", 1200);
        display_settings_.window_height = d.value("height", 800);
        display_settings_.screen_width = d.value("screen_width", 1920);
        display_settings_.screen_height = d.value("screen_height", 1080);
        display_settings_.window_x = d.value("window_x", 0);
        display_settings_.window_y = d.value("window_y", 0);
        display_settings_.window_maximized = d.value("window_maximized", false);
        display_settings_.auto_resize = d.value("auto_resize", true);
        display_settings_.use_dark_theme = d.value("use_dark_theme", true);
        display_settings_.font_size = d.value("font_size", 14.0f);
        display_settings_.font_family = d.value("font_family", "default");
        display_settings_.enable_animation = d.value("enable_animation", true);
        display_settings_.frame_rate_limit = d.value("frame_rate_limit", 60);
        display_settings_.center_window = d.value("center_window", true);
        display_settings_.min_window_width = d.value("min_window_width", 800);
        display_settings_.min_window_height = d.value("min_window_height", 600);
        display_settings_.dpi_scale = d.value("dpi_scale", 1.0f);
        display_settings_.margin = d.value("margin", 10);
        display_settings_.language = d.value("language", std::string("ru"));

        // Загружаем тему: приоритет у ключа "theme" (int), fallback на use_dark_theme (bool)
        if (d.contains("theme")) {
            current_theme_ = static_cast<ThemeType>(d.value("theme", 0));
        } else {
            current_theme_ = display_settings_.use_dark_theme ? ThemeType::Dark : ThemeType::Light;
        }
    }
}

void Settings::deserializeServerSettings(const json& j) {
    if (j.contains("server")) {
        auto& s = j["server"];
        server_settings_.host = s.value("host", "localhost");
        server_settings_.port = s.value("port", 8081);
        server_settings_.api_url = s.value("api_url", "http://localhost:8081");
        server_settings_.connection_timeout = s.value("connection_timeout", 30000);
        server_settings_.request_timeout = s.value("request_timeout", 60000);
        server_settings_.max_retries = s.value("max_retries", 3);
        server_settings_.verify_ssl = s.value("verify_ssl", true);
    }
}

void Settings::deserializeChatSettings(const json& j) {
    if (j.contains("chat")) {
        auto& c = j["chat"];
        chat_settings_.max_tokens = c.value("max_tokens", 2048);
        chat_settings_.temperature = c.value("temperature", 0.7f);
        chat_settings_.top_p = c.value("top_p", 0.9f);
        chat_settings_.top_k = c.value("top_k", 40);
        chat_settings_.min_p = c.value("min_p", 0.05f);
        chat_settings_.repeat_penalty = c.value("repeat_penalty", 1.1f);
        chat_settings_.presence_penalty = c.value("presence_penalty", 0.0f);
        chat_settings_.frequency_penalty = c.value("frequency_penalty", 0.0f);
        chat_settings_.mirostat_mode = c.value("mirostat_mode", 0);
        chat_settings_.mirostat_tau = c.value("mirostat_tau", 5.0f);
        chat_settings_.mirostat_eta = c.value("mirostat_eta", 0.1f);
        chat_settings_.stop_on_newline = c.value("stop_on_newline", false);
        chat_settings_.default_system_prompt = c.value("default_system_prompt", "You are a helpful assistant.");
        chat_settings_.threads = c.value("threads", 4);
        chat_settings_.n_ctx = c.value("n_ctx", 4096);
        chat_settings_.seed = c.value("seed", -1);
        chat_settings_.tfs_z = c.value("tfs_z", 1.0f);
        chat_settings_.typical_p = c.value("typical_p", 1.0f);
        chat_settings_.n_gpu_layers = c.value("n_gpu_layers", 0);
        chat_settings_.tensor_split = c.value("tensor_split", "");
        chat_settings_.numa = c.value("numa", "none");
        // Миграция mlock/no_mmap в GPU settings
        gpu_settings_.mlock = c.value("mlock", false);
        gpu_settings_.no_mmap = c.value("no_mmap", false);
        chat_settings_.lora_adapters = c.value("lora_adapters", std::vector<std::string>{});
        chat_settings_.lora_base = c.value("lora_base", "");
        chat_settings_.mmproj = c.value("mmproj", "");
        chat_settings_.grammar = c.value("grammar", "");
        chat_settings_.chat_template = c.value("chat_template", "");
        chat_settings_.embedding = c.value("embedding", false);
        chat_settings_.reverse_prompt = c.value("reverse_prompt", std::vector<std::string>{});
        chat_settings_.log_format = c.value("log_format", "text");
        chat_settings_.verbosity = c.value("verbosity", 0);
    }
}

void Settings::deserializeFileSettings(const json& j) {
    if (j.contains("files")) {
        auto& f = j["files"];
        file_settings_.default_save_path = f.value("default_save_path", "");
        file_settings_.auto_save_path = f.value("auto_save_path", "");
        file_settings_.auto_save_enabled = f.value("auto_save_enabled", false);
        file_settings_.auto_save_interval = f.value("auto_save_interval", 300);
    }
}

void Settings::deserializePerformanceSettings(const json& j) {
    if (j.contains("performance")) {
        auto& p = j["performance"];
        performance_settings_.enable_vsync = p.value("enable_vsync", true);
        performance_settings_.target_fps = p.value("target_fps", 60);
        performance_settings_.idle_fps = p.value("idle_fps", 15);
        performance_settings_.idle_timeout_ms = p.value("idle_timeout_ms", 5000);
        performance_settings_.enable_smart_redraw = p.value("enable_smart_redraw", true);
        performance_settings_.show_performance_overlay = p.value("show_performance_overlay", false);
        performance_settings_.performance_update_interval_ms = p.value("performance_update_interval_ms", 250);
        performance_settings_.enable_logging = p.value("enable_logging", true);
        performance_settings_.log_level = p.value("log_level", "Info");
        performance_settings_.log_to_file = p.value("log_to_file", false);
        performance_settings_.log_file_path = p.value("log_file_path", "llama-gui.log");
        performance_settings_.log_flush_policy = p.value("log_flush_policy", "Immediate");
        performance_settings_.debug_mode = p.value("debug_mode", false);
    }
}

void Settings::deserializeRagSettings(const json& j) {
    if (j.contains("rag")) {
        auto& r = j["rag"];
        rag_settings_.embedding_model_path = r.value("embedding_model_path", "");
        rag_settings_.embedding_server_url = r.value("embedding_server_url", "");
        rag_settings_.max_chunks_in_memory = r.value("max_chunks_in_memory", 100);
        rag_settings_.similarity_threshold = r.value("similarity_threshold", 0.7f);
        rag_settings_.max_embedding_cache_size = r.value("max_embedding_cache_size", 50);
        rag_settings_.embedding_dimension = r.value("embedding_dimension", 384);
        rag_settings_.max_sequence_length = r.value("max_sequence_length", 512);
        rag_settings_.max_tokens_per_chunk = r.value("max_tokens_per_chunk", 256);
        rag_settings_.search_k = r.value("search_k", 5);
        rag_settings_.mmr_lambda = r.value("mmr_lambda", 0.5f);
        rag_settings_.enable_mmr = r.value("enable_mmr", false);
        rag_settings_.enable_rag = r.value("enable_rag", true);
        rag_settings_.enable_caching = r.value("enable_caching", true);
        rag_settings_.rag_mode = static_cast<RagMode>(r.value("rag_mode", static_cast<int>(RagMode::DocumentsOnly)));

        // Параметры гибридного поиска
        rag_settings_.enable_hybrid_search = r.value("enable_hybrid_search", true);
        rag_settings_.keyword_boost_weight = r.value("keyword_boost_weight", 2.0f);
        rag_settings_.enable_query_expansion = r.value("enable_query_expansion", true);

        // Настройки глубокого анализа документа
        rag_settings_.deep_analysis.mode = static_cast<DeepAnalysisMode>(r.value("deep_analysis_mode", static_cast<int>(DeepAnalysisMode::Disabled)));
        rag_settings_.deep_analysis.chunks_per_batch = r.value("deep_analysis_chunks_per_batch", 10);
        rag_settings_.deep_analysis.max_iterations = r.value("deep_analysis_max_iterations", 50);
        rag_settings_.deep_analysis.enable_progressive_summary = r.value("deep_analysis_enable_progressive_summary", true);
        rag_settings_.deep_analysis.final_synthesis_chunks = r.value("deep_analysis_final_synthesis_chunks", 30);
        rag_settings_.deep_analysis.auto_adjust_context_size = r.value("deep_analysis_auto_adjust_context_size", true);
        rag_settings_.deep_analysis.target_context_size = r.value("deep_analysis_target_context_size", 8192);
    }
}

} // namespace core
} // namespace llama_gui
