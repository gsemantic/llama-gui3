#include "../include/core/settings.h"
#include "../include/core/ini_parser.h"
#include "../include/core/ini_section_parser.h"
#include <iostream>
#include <sstream>
#include <filesystem>

namespace llama_gui {
namespace core {

// =========================================================================
// Вспомогательные функции для конвертации типов
// =========================================================================

namespace {

template<typename T>
std::string to_string_impl(const T& value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_same_v<T, float>) {
        return std::to_string(value);
    } else if constexpr (std::is_same_v<T, int>) {
        return std::to_string(value);
    } else {
        return std::to_string(value);
    }
}

template<typename T>
void set_from_ini(const IniParser::Document& doc, const std::string& section,
                  const std::string& key, T& value) {
    // Реализация будет специализирована ниже
}

template<>
void set_from_ini<int>(const IniParser::Document& doc, const std::string& section,
                       const std::string& key, int& value) {
    int val = IniSectionParser::get_int(doc, section, key, value);
    value = val;
}

template<>
void set_from_ini<float>(const IniParser::Document& doc, const std::string& section,
                         const std::string& key, float& value) {
    float val = IniSectionParser::get_float(doc, section, key, value);
    value = val;
}

template<>
void set_from_ini<bool>(const IniParser::Document& doc, const std::string& section,
                        const std::string& key, bool& value) {
    bool val = IniSectionParser::get_bool(doc, section, key, value);
    value = val;
}

template<>
void set_from_ini<std::string>(const IniParser::Document& doc, const std::string& section,
                               const std::string& key, std::string& value) {
    std::string val = IniSectionParser::get_string(doc, section, key, value);
    value = val;
}

} // anonymous namespace

// =========================================================================
// Загрузка из INI
// =========================================================================

bool Settings::load_from_ini(const std::string& file_path) {
    IniParser::Document doc;

    if (!IniParser::load(file_path, doc)) {
        std::cerr << "Failed to load INI file: " << file_path << std::endl;
        return false;
    }

    std::cout << "Loading settings from INI: " << file_path << std::endl;

    load_display_settings(doc);
    load_performance_settings(doc);
    load_server_settings(doc);
    load_chat_settings(doc);
    load_file_settings(doc);
    load_rag_settings(doc);
    load_sampling_settings(doc);
    load_model_loading_settings(doc);
    load_gpu_settings(doc);
    load_cache_settings(doc);
    load_rope_settings(doc);
    load_control_vector_settings(doc);
    load_server_runtime_settings(doc);
    load_batch_settings(doc);
    load_grammar_settings(doc);
    load_output_settings(doc);

    std::cout << "Settings successfully loaded from INI file" << std::endl;

    sync_ctx_size();
    sync_max_tokens();
    save_to_ini(file_path);

    return true;
}

void Settings::load_display_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "display", "window_width", display_settings_.window_width);
    set_from_ini(doc, "display", "window_height", display_settings_.window_height);
    set_from_ini(doc, "display", "window_maximized", display_settings_.window_maximized);
    set_from_ini(doc, "display", "window_x", display_settings_.window_x);
    set_from_ini(doc, "display", "window_y", display_settings_.window_y);
    set_from_ini(doc, "display", "use_dark_theme", display_settings_.use_dark_theme);
    set_from_ini(doc, "display", "font_size", display_settings_.font_size);
    set_from_ini(doc, "display", "font_family", display_settings_.font_family);
    set_from_ini(doc, "display", "enable_animation", display_settings_.enable_animation);
    set_from_ini(doc, "display", "frame_rate_limit", display_settings_.frame_rate_limit);
    set_from_ini(doc, "display", "screen_width", display_settings_.screen_width);
    set_from_ini(doc, "display", "screen_height", display_settings_.screen_height);
    set_from_ini(doc, "display", "dpi_scale", display_settings_.dpi_scale);
    set_from_ini(doc, "display", "auto_resize", display_settings_.auto_resize);
    set_from_ini(doc, "display", "min_window_width", display_settings_.min_window_width);
    set_from_ini(doc, "display", "min_window_height", display_settings_.min_window_height);
    set_from_ini(doc, "display", "center_window", display_settings_.center_window);
    set_from_ini(doc, "display", "margin", display_settings_.margin);
    set_from_ini(doc, "display", "language", display_settings_.language);
}

void Settings::load_performance_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "performance", "enable_vsync", performance_settings_.enable_vsync);
    set_from_ini(doc, "performance", "target_fps", performance_settings_.target_fps);
    set_from_ini(doc, "performance", "idle_fps", performance_settings_.idle_fps);
    set_from_ini(doc, "performance", "idle_timeout_ms", performance_settings_.idle_timeout_ms);
    set_from_ini(doc, "performance", "enable_smart_redraw", performance_settings_.enable_smart_redraw);
    set_from_ini(doc, "performance", "show_performance_overlay", performance_settings_.show_performance_overlay);
    set_from_ini(doc, "performance", "performance_update_interval_ms", performance_settings_.performance_update_interval_ms);
    set_from_ini(doc, "performance", "enable_logging", performance_settings_.enable_logging);
    set_from_ini(doc, "performance", "log_level", performance_settings_.log_level);
    set_from_ini(doc, "performance", "log_to_file", performance_settings_.log_to_file);
    set_from_ini(doc, "performance", "log_file_path", performance_settings_.log_file_path);
    set_from_ini(doc, "performance", "log_flush_policy", performance_settings_.log_flush_policy);
    set_from_ini(doc, "performance", "debug_mode", performance_settings_.debug_mode);
}

void Settings::load_server_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "server", "host", server_settings_.host);
    set_from_ini(doc, "server", "port", server_settings_.port);
    set_from_ini(doc, "server", "api_url", server_settings_.api_url);
    set_from_ini(doc, "server", "connection_timeout", server_settings_.connection_timeout);
    set_from_ini(doc, "server", "request_timeout", server_settings_.request_timeout);
    set_from_ini(doc, "server", "max_retries", server_settings_.max_retries);
    set_from_ini(doc, "server", "verify_ssl", server_settings_.verify_ssl);
    set_from_ini(doc, "server", "auth_token", server_settings_.auth_token);
}

void Settings::load_chat_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "chat", "auto_scroll", chat_settings_.auto_scroll);
    set_from_ini(doc, "chat", "max_messages_display", chat_settings_.max_messages_display);
    set_from_ini(doc, "chat", "show_timestamps", chat_settings_.show_timestamps);
    set_from_ini(doc, "chat", "show_system_messages", chat_settings_.show_system_messages);
    set_from_ini(doc, "chat", "preserve_formatting", chat_settings_.preserve_formatting);
    set_from_ini(doc, "chat", "default_system_prompt", chat_settings_.default_system_prompt);
    set_from_ini(doc, "chat", "max_tokens", chat_settings_.max_tokens);
    set_from_ini(doc, "chat", "temperature", chat_settings_.temperature);
    set_from_ini(doc, "chat", "top_p", chat_settings_.top_p);
    set_from_ini(doc, "chat", "top_k", chat_settings_.top_k);
    set_from_ini(doc, "chat", "min_p", chat_settings_.min_p);
    set_from_ini(doc, "chat", "repeat_penalty", chat_settings_.repeat_penalty);
    set_from_ini(doc, "chat", "presence_penalty", chat_settings_.presence_penalty);
    set_from_ini(doc, "chat", "frequency_penalty", chat_settings_.frequency_penalty);
    set_from_ini(doc, "chat", "mirostat_mode", chat_settings_.mirostat_mode);
    set_from_ini(doc, "chat", "mirostat_tau", chat_settings_.mirostat_tau);
    set_from_ini(doc, "chat", "mirostat_eta", chat_settings_.mirostat_eta);
    set_from_ini(doc, "chat", "stop_on_newline", chat_settings_.stop_on_newline);
    set_from_ini(doc, "chat", "threads", chat_settings_.threads);
    set_from_ini(doc, "chat", "n_ctx", chat_settings_.n_ctx);
    set_from_ini(doc, "chat", "seed", chat_settings_.seed);
    set_from_ini(doc, "chat", "tfs_z", chat_settings_.tfs_z);
    set_from_ini(doc, "chat", "typical_p", chat_settings_.typical_p);
    set_from_ini(doc, "chat", "n_gpu_layers", chat_settings_.n_gpu_layers);
    set_from_ini(doc, "chat", "tensor_split", chat_settings_.tensor_split);
    set_from_ini(doc, "chat", "numa", chat_settings_.numa);
    set_from_ini(doc, "chat", "lora_base", chat_settings_.lora_base);
    set_from_ini(doc, "chat", "mmproj", chat_settings_.mmproj);
    set_from_ini(doc, "chat", "grammar", chat_settings_.grammar);
    set_from_ini(doc, "chat", "chat_template", chat_settings_.chat_template);
    set_from_ini(doc, "chat", "embedding", chat_settings_.embedding);
    set_from_ini(doc, "chat", "log_format", chat_settings_.log_format);
    set_from_ini(doc, "chat", "verbosity", chat_settings_.verbosity);
}

void Settings::load_file_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "files", "default_save_path", file_settings_.default_save_path);
    set_from_ini(doc, "files", "default_export_path", file_settings_.default_export_path);
    set_from_ini(doc, "files", "auto_save_path", file_settings_.auto_save_path);
    set_from_ini(doc, "files", "auto_save_enabled", file_settings_.auto_save_enabled);
    set_from_ini(doc, "files", "auto_save_interval", file_settings_.auto_save_interval);
    set_from_ini(doc, "files", "max_file_size", file_settings_.max_file_size);
}

void Settings::load_rag_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "rag", "embedding_model_path", rag_settings_.embedding_model_path);
    set_from_ini(doc, "rag", "embedding_server_url", rag_settings_.embedding_server_url);
    set_from_ini(doc, "rag", "max_chunks_in_memory", rag_settings_.max_chunks_in_memory);
    set_from_ini(doc, "rag", "similarity_threshold", rag_settings_.similarity_threshold);
    set_from_ini(doc, "rag", "max_embedding_cache_size", rag_settings_.max_embedding_cache_size);
    set_from_ini(doc, "rag", "embedding_dimension", rag_settings_.embedding_dimension);
    set_from_ini(doc, "rag", "max_sequence_length", rag_settings_.max_sequence_length);
    set_from_ini(doc, "rag", "max_tokens_per_chunk", rag_settings_.max_tokens_per_chunk);
    set_from_ini(doc, "rag", "search_k", rag_settings_.search_k);
    set_from_ini(doc, "rag", "mmr_lambda", rag_settings_.mmr_lambda);
    set_from_ini(doc, "rag", "enable_mmr", rag_settings_.enable_mmr);
    set_from_ini(doc, "rag", "enable_rag", rag_settings_.enable_rag);
    set_from_ini(doc, "rag", "enable_caching", rag_settings_.enable_caching);
    int rag_mode_val = IniParser::get_int(doc, "rag", "rag_mode", static_cast<int>(rag_settings_.rag_mode));
    rag_settings_.rag_mode = static_cast<RagMode>(rag_mode_val);
    set_from_ini(doc, "rag", "enable_hybrid_search", rag_settings_.enable_hybrid_search);
    set_from_ini(doc, "rag", "keyword_boost_weight", rag_settings_.keyword_boost_weight);
    set_from_ini(doc, "rag", "enable_query_expansion", rag_settings_.enable_query_expansion);
    set_from_ini(doc, "rag", "deep_analysis_mode", rag_settings_.deep_analysis.mode);
    set_from_ini(doc, "rag", "deep_analysis_chunks_per_batch", rag_settings_.deep_analysis.chunks_per_batch);
    set_from_ini(doc, "rag", "deep_analysis_max_iterations", rag_settings_.deep_analysis.max_iterations);
    set_from_ini(doc, "rag", "deep_analysis_enable_progressive_summary", rag_settings_.deep_analysis.enable_progressive_summary);
    set_from_ini(doc, "rag", "deep_analysis_final_synthesis_chunks", rag_settings_.deep_analysis.final_synthesis_chunks);
    set_from_ini(doc, "rag", "deep_analysis_auto_adjust_context_size", rag_settings_.deep_analysis.auto_adjust_context_size);
    set_from_ini(doc, "rag", "deep_analysis_target_context_size", rag_settings_.deep_analysis.target_context_size);
}

void Settings::load_sampling_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "sampling", "temperature", sampling_settings_.temperature);
    set_from_ini(doc, "sampling", "top_k", sampling_settings_.top_k);
    set_from_ini(doc, "sampling", "top_p", sampling_settings_.top_p);
    set_from_ini(doc, "sampling", "min_p", sampling_settings_.min_p);
    set_from_ini(doc, "sampling", "typical_p", sampling_settings_.typical_p);
    set_from_ini(doc, "sampling", "tfs_z", sampling_settings_.tfs_z);
    set_from_ini(doc, "sampling", "xtc_probability", sampling_settings_.xtc_probability);
    set_from_ini(doc, "sampling", "xtc_threshold", sampling_settings_.xtc_threshold);
    set_from_ini(doc, "sampling", "dry_multiplier", sampling_settings_.dry_multiplier);
    set_from_ini(doc, "sampling", "dry_base", sampling_settings_.dry_base);
    set_from_ini(doc, "sampling", "dry_allowed_length", sampling_settings_.dry_allowed_length);
    set_from_ini(doc, "sampling", "dry_penalty_last_n", sampling_settings_.dry_penalty_last_n);
    set_from_ini(doc, "sampling", "dynatemp_range", sampling_settings_.dynatemp_range);
    set_from_ini(doc, "sampling", "dynatemp_exp", sampling_settings_.dynatemp_exp);
    set_from_ini(doc, "sampling", "repeat_penalty", sampling_settings_.repeat_penalty);
    set_from_ini(doc, "sampling", "presence_penalty", sampling_settings_.presence_penalty);
    set_from_ini(doc, "sampling", "frequency_penalty", sampling_settings_.frequency_penalty);
    set_from_ini(doc, "sampling", "repeat_last_n", sampling_settings_.repeat_last_n);
    set_from_ini(doc, "sampling", "penalize_nl", sampling_settings_.penalize_nl);
    set_from_ini(doc, "sampling", "ignore_eos", sampling_settings_.ignore_eos);
    set_from_ini(doc, "sampling", "mirostat_mode", sampling_settings_.mirostat_mode);
    set_from_ini(doc, "sampling", "mirostat_tau", sampling_settings_.mirostat_tau);
    set_from_ini(doc, "sampling", "mirostat_eta", sampling_settings_.mirostat_eta);
    set_from_ini(doc, "sampling", "samplers_order", sampling_settings_.samplers_order);
    set_from_ini(doc, "sampling", "use_custom_sampler_order", sampling_settings_.use_custom_sampler_order);
}

void Settings::load_model_loading_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "model_loading", "model_path", model_loading_settings_.model_path);
    set_from_ini(doc, "model_loading", "model_url", model_loading_settings_.model_url);
    set_from_ini(doc, "model_loading", "hf_repo", model_loading_settings_.hf_repo);
    set_from_ini(doc, "model_loading", "hf_file", model_loading_settings_.hf_file);
    set_from_ini(doc, "model_loading", "hf_token", model_loading_settings_.hf_token);
    set_from_ini(doc, "model_loading", "model_alias", model_loading_settings_.model_alias);
    set_from_ini(doc, "model_loading", "model_draft", model_loading_settings_.model_draft);
    set_from_ini(doc, "model_loading", "hf_repo_draft", model_loading_settings_.hf_repo_draft);
    set_from_ini(doc, "model_loading", "draft_max", model_loading_settings_.draft_max);
    set_from_ini(doc, "model_loading", "draft_min", model_loading_settings_.draft_min);
    set_from_ini(doc, "model_loading", "draft_p_min", model_loading_settings_.draft_p_min);
    set_from_ini(doc, "model_loading", "model_vocoder", model_loading_settings_.model_vocoder);
    set_from_ini(doc, "model_loading", "hf_repo_vocoder", model_loading_settings_.hf_repo_vocoder);
    set_from_ini(doc, "model_loading", "hf_file_vocoder", model_loading_settings_.hf_file_vocoder);
    set_from_ini(doc, "model_loading", "lora_base", model_loading_settings_.lora_base);
    set_from_ini(doc, "model_loading", "lora_init_without_apply", model_loading_settings_.lora_init_without_apply);
    set_from_ini(doc, "model_loading", "mmproj", model_loading_settings_.mmproj);
    set_from_ini(doc, "model_loading", "mmproj_url", model_loading_settings_.mmproj_url);
    set_from_ini(doc, "model_loading", "no_mmproj", model_loading_settings_.no_mmproj);
    set_from_ini(doc, "model_loading", "no_mmproj_offload", model_loading_settings_.no_mmproj_offload);
    set_from_ini(doc, "model_loading", "check_tensors", model_loading_settings_.check_tensors);
    set_from_ini(doc, "model_loading", "device", model_loading_settings_.device);
    set_from_ini(doc, "model_loading", "device_draft", model_loading_settings_.device_draft);
    set_from_ini(doc, "model_loading", "list_devices", model_loading_settings_.list_devices);
}

void Settings::load_gpu_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "gpu", "n_gpu_layers", gpu_settings_.n_gpu_layers);
    set_from_ini(doc, "gpu", "n_gpu_layers_draft", gpu_settings_.n_gpu_layers_draft);
    std::string split_mode_str = IniParser::get(doc, "gpu", "split_mode", "layer");
    gpu_settings_.set_split_mode(split_mode_str);
    set_from_ini(doc, "gpu", "tensor_split", gpu_settings_.tensor_split);
    set_from_ini(doc, "gpu", "main_gpu", gpu_settings_.main_gpu);
    set_from_ini(doc, "gpu", "no_op_offload", gpu_settings_.no_op_offload);
    set_from_ini(doc, "gpu", "no_kv_offload", gpu_settings_.no_kv_offload);
    set_from_ini(doc, "gpu", "no_warmup", gpu_settings_.no_warmup);
    set_from_ini(doc, "gpu", "mlock", gpu_settings_.mlock);
    set_from_ini(doc, "gpu", "no_mmap", gpu_settings_.no_mmap);
    int flash_attn_val = IniParser::get_int(doc, "gpu", "flash_attn", 0);
    if (flash_attn_val == 0) {
        gpu_settings_.flash_attn = GPUSettings::FlashAttention::Auto;
    } else if (flash_attn_val == 1) {
        gpu_settings_.flash_attn = GPUSettings::FlashAttention::Enabled;
    } else {
        gpu_settings_.flash_attn = GPUSettings::FlashAttention::Disabled;
    }
    set_from_ini(doc, "gpu", "defrag_thold", gpu_settings_.defrag_thold);
}

void Settings::load_cache_settings(const IniParser::Document& doc) {
    std::string cache_type_k_str = IniParser::get(doc, "cache", "cache_type_k", "f16");
    cache_settings_.cache_type_k = CacheSettings::cache_type_from_string(cache_type_k_str);
    std::string cache_type_v_str = IniParser::get(doc, "cache", "cache_type_v", "f16");
    cache_settings_.cache_type_v = CacheSettings::cache_type_from_string(cache_type_v_str);
    std::string cache_type_k_draft_str = IniParser::get(doc, "cache", "cache_type_k_draft", "f16");
    cache_settings_.cache_type_k_draft = CacheSettings::cache_type_from_string(cache_type_k_draft_str);
    std::string cache_type_v_draft_str = IniParser::get(doc, "cache", "cache_type_v_draft", "f16");
    cache_settings_.cache_type_v_draft = CacheSettings::cache_type_from_string(cache_type_v_draft_str);
    set_from_ini(doc, "cache", "cache_prompt", cache_settings_.cache_prompt);
    set_from_ini(doc, "cache", "cache_reuse", cache_settings_.cache_reuse);
    set_from_ini(doc, "cache", "swa_full", cache_settings_.swa_full);
    set_from_ini(doc, "cache", "no_context_shift", cache_settings_.no_context_shift);
    set_from_ini(doc, "cache", "slot_save_path", cache_settings_.slot_save_path);
    set_from_ini(doc, "cache", "slot_prompt_similarity", cache_settings_.slot_prompt_similarity);
    set_from_ini(doc, "cache", "slots_endpoint_enabled", cache_settings_.slots_endpoint_enabled);
}

void Settings::load_rope_settings(const IniParser::Document& doc) {
    std::string rope_scaling_str = IniParser::get(doc, "rope", "rope_scaling", "linear");
    rope_settings_.set_scaling(rope_scaling_str);
    set_from_ini(doc, "rope", "rope_scale", rope_settings_.rope_scale);
    set_from_ini(doc, "rope", "rope_freq_base", rope_settings_.rope_freq_base);
    set_from_ini(doc, "rope", "rope_freq_scale", rope_settings_.rope_freq_scale);
    set_from_ini(doc, "rope", "yarn_orig_ctx", rope_settings_.yarn_orig_ctx);
    set_from_ini(doc, "rope", "yarn_ext_factor", rope_settings_.yarn_ext_factor);
    set_from_ini(doc, "rope", "yarn_attn_factor", rope_settings_.yarn_attn_factor);
    set_from_ini(doc, "rope", "yarn_beta_slow", rope_settings_.yarn_beta_slow);
    set_from_ini(doc, "rope", "yarn_beta_fast", rope_settings_.yarn_beta_fast);
}

void Settings::load_control_vector_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "control_vector", "control_vector_layer_start", control_vector_settings_.control_vector_layer_start);
    set_from_ini(doc, "control_vector", "control_vector_layer_end", control_vector_settings_.control_vector_layer_end);
}

void Settings::load_server_runtime_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "server_runtime", "host", server_runtime_settings_.host);
    set_from_ini(doc, "server_runtime", "port", server_runtime_settings_.port);
    set_from_ini(doc, "server_runtime", "timeout", server_runtime_settings_.timeout);
    set_from_ini(doc, "server_runtime", "api_prefix", server_runtime_settings_.api_prefix);
    set_from_ini(doc, "server_runtime", "offline", server_runtime_settings_.offline);
    set_from_ini(doc, "server_runtime", "threads_http", server_runtime_settings_.threads_http);
    set_from_ini(doc, "server_runtime", "static_path", server_runtime_settings_.static_path);
    set_from_ini(doc, "server_runtime", "no_webui", server_runtime_settings_.no_webui);
    set_from_ini(doc, "server_runtime", "api_key_file", server_runtime_settings_.api_key_file);
    set_from_ini(doc, "server_runtime", "ssl_key_file", server_runtime_settings_.ssl_key_file);
    set_from_ini(doc, "server_runtime", "ssl_cert_file", server_runtime_settings_.ssl_cert_file);
    set_from_ini(doc, "server_runtime", "embeddings_mode", server_runtime_settings_.embeddings_mode);
    set_from_ini(doc, "server_runtime", "reranking_mode", server_runtime_settings_.reranking_mode);
    set_from_ini(doc, "server_runtime", "metrics_enabled", server_runtime_settings_.metrics_enabled);
    set_from_ini(doc, "server_runtime", "slot_save_path", server_runtime_settings_.slot_save_path);
    set_from_ini(doc, "server_runtime", "n_parallel", server_runtime_settings_.n_parallel);
    set_from_ini(doc, "server_runtime", "cache_type_k", server_runtime_settings_.cache_type_k);
    set_from_ini(doc, "server_runtime", "cache_type_v", server_runtime_settings_.cache_type_v);
    set_from_ini(doc, "server_runtime", "cache_reuse", server_runtime_settings_.cache_reuse);
    set_from_ini(doc, "server_runtime", "log_disabled", server_runtime_settings_.log_disabled);
    set_from_ini(doc, "server_runtime", "log_file", server_runtime_settings_.log_file);
    set_from_ini(doc, "server_runtime", "log_colors", server_runtime_settings_.log_colors);
    set_from_ini(doc, "server_runtime", "log_verbose", server_runtime_settings_.log_verbose);
    {
        std::string log_verbosity_str = std::to_string(server_runtime_settings_.log_verbosity);
        set_from_ini(doc, "server_runtime", "log_verbosity", log_verbosity_str);
    }
    set_from_ini(doc, "server_runtime", "log_format", server_runtime_settings_.log_format);
    {
        std::string log_prefix_str = server_runtime_settings_.log_prefix ? "true" : "false";
        set_from_ini(doc, "server_runtime", "log_prefix", log_prefix_str);
    }
    {
        std::string log_timestamps_str = server_runtime_settings_.log_timestamps ? "true" : "false";
        set_from_ini(doc, "server_runtime", "log_timestamps", log_timestamps_str);
    }
}

void Settings::load_batch_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "batch", "batch_size", batch_settings_.batch_size);
    set_from_ini(doc, "batch", "ubatch_size", batch_settings_.ubatch_size);
    set_from_ini(doc, "batch", "ctx_size", batch_settings_.ctx_size);
    set_from_ini(doc, "batch", "ctx_size_draft", batch_settings_.ctx_size_draft);
    set_from_ini(doc, "batch", "threads", batch_settings_.threads);
    set_from_ini(doc, "batch", "threads_batch", batch_settings_.threads_batch);
    set_from_ini(doc, "batch", "cpu_mask", batch_settings_.cpu_mask);
    set_from_ini(doc, "batch", "cpu_range", batch_settings_.cpu_range);
    set_from_ini(doc, "batch", "cpu_strict", batch_settings_.cpu_strict);
    set_from_ini(doc, "batch", "priority", batch_settings_.priority);
    set_from_ini(doc, "batch", "poll_level", batch_settings_.poll_level);
    set_from_ini(doc, "batch", "cpu_mask_batch", batch_settings_.cpu_mask_batch);
    set_from_ini(doc, "batch", "cpu_range_batch", batch_settings_.cpu_range_batch);
    set_from_ini(doc, "batch", "cpu_strict_batch", batch_settings_.cpu_strict_batch);
    set_from_ini(doc, "batch", "priority_batch", batch_settings_.priority_batch);
    set_from_ini(doc, "batch", "poll_batch", batch_settings_.poll_batch);
    set_from_ini(doc, "batch", "cont_batching", batch_settings_.cont_batching);
    set_from_ini(doc, "batch", "no_perf", batch_settings_.no_perf);
}

void Settings::load_grammar_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "grammar", "grammar", grammar_settings_.grammar);
    set_from_ini(doc, "grammar", "grammar_file", grammar_settings_.grammar_file);
    set_from_ini(doc, "grammar", "json_schema", grammar_settings_.json_schema);
    set_from_ini(doc, "grammar", "json_schema_file", grammar_settings_.json_schema_file);
    set_from_ini(doc, "grammar", "chat_template", grammar_settings_.chat_template);
    set_from_ini(doc, "grammar", "chat_template_file", grammar_settings_.chat_template_file);
    set_from_ini(doc, "grammar", "chat_template_kwargs", grammar_settings_.chat_template_kwargs);
    set_from_ini(doc, "grammar", "use_jinja", grammar_settings_.use_jinja);
    set_from_ini(doc, "grammar", "no_prefill_assistant", grammar_settings_.no_prefill_assistant);
    set_from_ini(doc, "grammar", "system_prompt_file", grammar_settings_.system_prompt_file);
    set_from_ini(doc, "grammar", "default_system_prompt", grammar_settings_.default_system_prompt);
    std::string reasoning_format_str = IniParser::get(doc, "grammar", "reasoning_format", "none");
    if (reasoning_format_str == "deepseek") {
        grammar_settings_.reasoning_format = GrammarSettings::ReasoningFormat::Deepseek;
    } else {
        grammar_settings_.reasoning_format = GrammarSettings::ReasoningFormat::None;
    }
    set_from_ini(doc, "grammar", "reasoning_budget", grammar_settings_.reasoning_budget);
}

void Settings::load_output_settings(const IniParser::Document& doc) {
    set_from_ini(doc, "output", "n_predict", output_settings_.n_predict);
    set_from_ini(doc, "output", "keep", output_settings_.keep);
    set_from_ini(doc, "output", "special_tokens", output_settings_.special_tokens);
    set_from_ini(doc, "output", "spm_infill", output_settings_.spm_infill);
    set_from_ini(doc, "output", "verbose_prompt", output_settings_.verbose_prompt);
    set_from_ini(doc, "output", "escape_sequences", output_settings_.escape_sequences);
    set_from_ini(doc, "output", "tts_use_guide_tokens", output_settings_.tts_use_guide_tokens);
    set_from_ini(doc, "output", "pooling_type", output_settings_.pooling_type);
}

// =========================================================================
// Сохранение в INI
// =========================================================================

bool Settings::save_to_ini(const std::string& file_path) const {
    using IniDoc = IniParser::Document;
    IniDoc doc;

    bool result = IniParser::save(file_path, doc);
    if (!result) {
        std::cerr << "Failed to save INI file: " << file_path << std::endl;
    }

    return result;
}

} // namespace core
} // namespace llama_gui
