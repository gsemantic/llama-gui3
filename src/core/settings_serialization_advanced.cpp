#include "../include/core/settings.h"
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// =========================================================================
// Сериализация расширенных настроек llama.cpp (sampling, model_loading, gpu, cache, rope)
// =========================================================================

void Settings::serializeSamplingSettings(json& j) const {
    j["sampling"] = {
        {"temperature", sampling_settings_.temperature},
        {"top_k", sampling_settings_.top_k},
        {"top_p", sampling_settings_.top_p},
        {"min_p", sampling_settings_.min_p},
        {"typical_p", sampling_settings_.typical_p},
        {"tfs_z", sampling_settings_.tfs_z},
        {"xtc_probability", sampling_settings_.xtc_probability},
        {"xtc_threshold", sampling_settings_.xtc_threshold},
        {"dry_multiplier", sampling_settings_.dry_multiplier},
        {"dry_base", sampling_settings_.dry_base},
        {"dry_allowed_length", sampling_settings_.dry_allowed_length},
        {"dry_penalty_last_n", sampling_settings_.dry_penalty_last_n},
        {"dry_sequence_breakers", sampling_settings_.dry_sequence_breakers},
        {"dynatemp_range", sampling_settings_.dynatemp_range},
        {"dynatemp_exp", sampling_settings_.dynatemp_exp},
        {"repeat_penalty", sampling_settings_.repeat_penalty},
        {"presence_penalty", sampling_settings_.presence_penalty},
        {"frequency_penalty", sampling_settings_.frequency_penalty},
        {"repeat_last_n", sampling_settings_.repeat_last_n},
        {"penalize_nl", sampling_settings_.penalize_nl},
        {"ignore_eos", sampling_settings_.ignore_eos},
        {"mirostat_mode", sampling_settings_.mirostat_mode},
        {"mirostat_tau", sampling_settings_.mirostat_tau},
        {"mirostat_eta", sampling_settings_.mirostat_eta},
        {"samplers_order", sampling_settings_.samplers_order},
        {"use_custom_sampler_order", sampling_settings_.use_custom_sampler_order}
    };
}

void Settings::serializeModelLoadingSettings(json& j) const {
    j["model_loading"] = {
        {"model_path", model_loading_settings_.model_path},
        {"model_url", model_loading_settings_.model_url},
        {"hf_repo", model_loading_settings_.hf_repo},
        {"hf_file", model_loading_settings_.hf_file},
        {"hf_token", model_loading_settings_.hf_token},
        {"model_alias", model_loading_settings_.model_alias},
        {"model_draft", model_loading_settings_.model_draft},
        {"hf_repo_draft", model_loading_settings_.hf_repo_draft},
        {"draft_max", model_loading_settings_.draft_max},
        {"draft_min", model_loading_settings_.draft_min},
        {"draft_p_min", model_loading_settings_.draft_p_min},
        {"model_vocoder", model_loading_settings_.model_vocoder},
        {"hf_repo_vocoder", model_loading_settings_.hf_repo_vocoder},
        {"hf_file_vocoder", model_loading_settings_.hf_file_vocoder},
        {"lora_base", model_loading_settings_.lora_base},
        {"lora_init_without_apply", model_loading_settings_.lora_init_without_apply},
        {"mmproj", model_loading_settings_.mmproj},
        {"mmproj_url", model_loading_settings_.mmproj_url},
        {"no_mmproj", model_loading_settings_.no_mmproj},
        {"no_mmproj_offload", model_loading_settings_.no_mmproj_offload},
        {"check_tensors", model_loading_settings_.check_tensors},
        {"device", model_loading_settings_.device},
        {"device_draft", model_loading_settings_.device_draft},
        {"list_devices", model_loading_settings_.list_devices}
    };

    // LoRA adapters (отдельно как массив объектов)
    j["lora_adapters"] = json::array();
    for (const auto& adapter : model_loading_settings_.lora_adapters) {
        j["lora_adapters"].push_back({{"path", adapter.path}, {"scale", adapter.scale}});
    }

    // Override KV
    j["override_kv"] = model_loading_settings_.override_kv;
}

void Settings::serializeGpuSettings(json& j) const {
    j["gpu"] = {
        {"n_gpu_layers", gpu_settings_.n_gpu_layers},
        {"n_gpu_layers_draft", gpu_settings_.n_gpu_layers_draft},
        {"split_mode", gpu_settings_.get_split_mode_string()},
        {"tensor_split", gpu_settings_.tensor_split},
        {"main_gpu", gpu_settings_.main_gpu},
        {"no_op_offload", gpu_settings_.no_op_offload},
        {"no_kv_offload", gpu_settings_.no_kv_offload},
        {"no_warmup", gpu_settings_.no_warmup},
        {"flash_attn", static_cast<int>(gpu_settings_.flash_attn)},
        {"defrag_thold", gpu_settings_.defrag_thold}
    };
}

void Settings::serializeCacheSettings(json& j) const {
    j["cache"] = {
        {"cache_type_k", CacheSettings::cache_type_to_string(cache_settings_.cache_type_k)},
        {"cache_type_v", CacheSettings::cache_type_to_string(cache_settings_.cache_type_v)},
        {"cache_type_k_draft", CacheSettings::cache_type_to_string(cache_settings_.cache_type_k_draft)},
        {"cache_type_v_draft", CacheSettings::cache_type_to_string(cache_settings_.cache_type_v_draft)},
        {"cache_prompt", cache_settings_.cache_prompt},
        {"cache_reuse", cache_settings_.cache_reuse},
        {"swa_full", cache_settings_.swa_full},
        {"no_context_shift", cache_settings_.no_context_shift},
        {"slot_save_path", cache_settings_.slot_save_path},
        {"slot_prompt_similarity", cache_settings_.slot_prompt_similarity},
        {"slots_endpoint_enabled", cache_settings_.slots_endpoint_enabled}
    };
}

void Settings::serializeRopeSettings(json& j) const {
    j["rope"] = {
        {"rope_scaling", rope_settings_.get_scaling_string()},
        {"rope_scale", rope_settings_.rope_scale},
        {"rope_freq_base", rope_settings_.rope_freq_base},
        {"rope_freq_scale", rope_settings_.rope_freq_scale},
        {"yarn_orig_ctx", rope_settings_.yarn_orig_ctx},
        {"yarn_ext_factor", rope_settings_.yarn_ext_factor},
        {"yarn_attn_factor", rope_settings_.yarn_attn_factor},
        {"yarn_beta_slow", rope_settings_.yarn_beta_slow},
        {"yarn_beta_fast", rope_settings_.yarn_beta_fast}
    };
}

void Settings::serializeControlVectorSettings(json& j) const {
    // Control vectors
    j["control_vectors"] = json::array();
    for (const auto& cv : control_vector_settings_.control_vectors) {
        j["control_vectors"].push_back({{"path", cv.path}, {"scale", cv.scale}});
    }
    j["control_vector_layer_start"] = control_vector_settings_.control_vector_layer_start;
    j["control_vector_layer_end"] = control_vector_settings_.control_vector_layer_end;
}

void Settings::serializeTensorOverrideSettings(json& j) const {
    // Tensor overrides
    j["tensor_overrides"] = json::array();
    for (const auto& ov : tensor_override_settings_.overrides) {
        j["tensor_overrides"].push_back({{"pattern", ov.pattern}, {"buffer_type", ov.buffer_type}});
    }
}

// =========================================================================
// Десериализация расширенных настроек llama.cpp
// =========================================================================

void Settings::deserializeSamplingSettings(const json& j) {
    if (j.contains("sampling")) {
        auto& s = j["sampling"];
        sampling_settings_.temperature = s.value("temperature", 0.8f);
        sampling_settings_.top_k = s.value("top_k", 40);
        sampling_settings_.top_p = s.value("top_p", 0.9f);
        sampling_settings_.min_p = s.value("min_p", 0.1f);
        sampling_settings_.typical_p = s.value("typical_p", 1.0f);
        sampling_settings_.tfs_z = s.value("tfs_z", 1.0f);
        sampling_settings_.xtc_probability = s.value("xtc_probability", 0.0f);
        sampling_settings_.xtc_threshold = s.value("xtc_threshold", 0.1f);
        sampling_settings_.dry_multiplier = s.value("dry_multiplier", 0.0f);
        sampling_settings_.dry_base = s.value("dry_base", 1.75f);
        sampling_settings_.dry_allowed_length = s.value("dry_allowed_length", 2);
        sampling_settings_.dry_penalty_last_n = s.value("dry_penalty_last_n", -1);
        sampling_settings_.dry_sequence_breakers = s.value("dry_sequence_breakers", std::vector<std::string>{"\n", ":", "\"", "*"});
        sampling_settings_.dynatemp_range = s.value("dynatemp_range", 0.0f);
        sampling_settings_.dynatemp_exp = s.value("dynatemp_exp", 1.0f);
        sampling_settings_.repeat_penalty = s.value("repeat_penalty", 1.1f);
        sampling_settings_.presence_penalty = s.value("presence_penalty", 0.0f);
        sampling_settings_.frequency_penalty = s.value("frequency_penalty", 0.0f);
        sampling_settings_.repeat_last_n = s.value("repeat_last_n", 64);
        sampling_settings_.penalize_nl = s.value("penalize_nl", true);
        sampling_settings_.ignore_eos = s.value("ignore_eos", false);
        sampling_settings_.mirostat_mode = s.value("mirostat_mode", 0);
        sampling_settings_.mirostat_tau = s.value("mirostat_tau", 5.0f);
        sampling_settings_.mirostat_eta = s.value("mirostat_eta", 0.1f);
        sampling_settings_.samplers_order = s.value("samplers_order", "edskypmxt");
        sampling_settings_.use_custom_sampler_order = s.value("use_custom_sampler_order", false);
    }
}

void Settings::deserializeModelLoadingSettings(const json& j) {
    if (j.contains("model_loading")) {
        auto& m = j["model_loading"];
        model_loading_settings_.model_path = m.value("model_path", "");
        model_loading_settings_.model_url = m.value("model_url", "");
        model_loading_settings_.hf_repo = m.value("hf_repo", "");
        model_loading_settings_.hf_file = m.value("hf_file", "");
        model_loading_settings_.hf_token = m.value("hf_token", "");
        model_loading_settings_.model_alias = m.value("model_alias", "");
        model_loading_settings_.model_draft = m.value("model_draft", "");
        model_loading_settings_.hf_repo_draft = m.value("hf_repo_draft", "");
        model_loading_settings_.draft_max = m.value("draft_max", 16);
        model_loading_settings_.draft_min = m.value("draft_min", 0);
        model_loading_settings_.draft_p_min = m.value("draft_p_min", 0.8f);
        model_loading_settings_.model_vocoder = m.value("model_vocoder", "");
        model_loading_settings_.hf_repo_vocoder = m.value("hf_repo_vocoder", "");
        model_loading_settings_.hf_file_vocoder = m.value("hf_file_vocoder", "");
        model_loading_settings_.lora_base = m.value("lora_base", "");
        model_loading_settings_.lora_init_without_apply = m.value("lora_init_without_apply", false);
        model_loading_settings_.mmproj = m.value("mmproj", "");
        model_loading_settings_.mmproj_url = m.value("mmproj_url", "");
        model_loading_settings_.no_mmproj = m.value("no_mmproj", false);
        model_loading_settings_.no_mmproj_offload = m.value("no_mmproj_offload", false);
        model_loading_settings_.check_tensors = m.value("check_tensors", false);
        model_loading_settings_.device = m.value("device", "");
        model_loading_settings_.device_draft = m.value("device_draft", "");
        model_loading_settings_.list_devices = m.value("list_devices", false);
    }

    // LoRA adapters
    if (j.contains("lora_adapters")) {
        model_loading_settings_.lora_adapters.clear();
        for (const auto& adapter : j["lora_adapters"]) {
            ModelLoadingSettings::LoRAAdapter lora;
            lora.path = adapter.value("path", "");
            lora.scale = adapter.value("scale", 1.0f);
            model_loading_settings_.lora_adapters.push_back(lora);
        }
    }

    // Override KV
    if (j.contains("override_kv")) {
        model_loading_settings_.override_kv = j["override_kv"].get<std::map<std::string, std::string>>();
    }
}

void Settings::deserializeGpuSettings(const json& j) {
    if (j.contains("gpu")) {
        auto& g = j["gpu"];
        gpu_settings_.n_gpu_layers = g.value("n_gpu_layers", 0);
        gpu_settings_.n_gpu_layers_draft = g.value("n_gpu_layers_draft", -1);
        gpu_settings_.set_split_mode(g.value("split_mode", "layer"));
        gpu_settings_.tensor_split = g.value("tensor_split", "");
        gpu_settings_.main_gpu = g.value("main_gpu", 0);
        gpu_settings_.no_op_offload = g.value("no_op_offload", false);
        gpu_settings_.no_kv_offload = g.value("no_kv_offload", false);
        gpu_settings_.no_warmup = g.value("no_warmup", false);
        gpu_settings_.flash_attn = static_cast<GPUSettings::FlashAttention>(g.value("flash_attn", 0));
        gpu_settings_.defrag_thold = g.value("defrag_thold", 0.1f);
    }
}

void Settings::deserializeCacheSettings(const json& j) {
    if (j.contains("cache")) {
        auto& c = j["cache"];
        cache_settings_.cache_type_k = CacheSettings::cache_type_from_string(c.value("cache_type_k", "f16"));
        cache_settings_.cache_type_v = CacheSettings::cache_type_from_string(c.value("cache_type_v", "f16"));
        cache_settings_.cache_type_k_draft = CacheSettings::cache_type_from_string(c.value("cache_type_k_draft", "f16"));
        cache_settings_.cache_type_v_draft = CacheSettings::cache_type_from_string(c.value("cache_type_v_draft", "f16"));
        cache_settings_.cache_prompt = c.value("cache_prompt", true);
        cache_settings_.cache_reuse = c.value("cache_reuse", 0);
        cache_settings_.swa_full = c.value("swa_full", false);
        cache_settings_.no_context_shift = c.value("no_context_shift", false);
        cache_settings_.slot_save_path = c.value("slot_save_path", "");
        cache_settings_.slot_prompt_similarity = c.value("slot_prompt_similarity", 0.5f);
        cache_settings_.slots_endpoint_enabled = c.value("slots_endpoint_enabled", true);
    }
}

void Settings::deserializeRopeSettings(const json& j) {
    if (j.contains("rope")) {
        auto& r = j["rope"];
        rope_settings_.set_scaling(r.value("rope_scaling", "linear"));
        rope_settings_.rope_scale = r.value("rope_scale", 1.0f);
        rope_settings_.rope_freq_base = r.value("rope_freq_base", 0.0f);
        rope_settings_.rope_freq_scale = r.value("rope_freq_scale", 1.0f);
        rope_settings_.yarn_orig_ctx = r.value("yarn_orig_ctx", 0);
        rope_settings_.yarn_ext_factor = r.value("yarn_ext_factor", -1.0f);
        rope_settings_.yarn_attn_factor = r.value("yarn_attn_factor", 1.0f);
        rope_settings_.yarn_beta_slow = r.value("yarn_beta_slow", 1.0f);
        rope_settings_.yarn_beta_fast = r.value("yarn_beta_fast", 32.0f);
    }
}

void Settings::deserializeControlVectorSettings(const json& j) {
    // Control vectors
    if (j.contains("control_vectors")) {
        control_vector_settings_.control_vectors.clear();
        for (const auto& cv : j["control_vectors"]) {
            ControlVectorSettings::ControlVector control;
            control.path = cv.value("path", "");
            control.scale = cv.value("scale", 1.0f);
            control_vector_settings_.control_vectors.push_back(control);
        }
    }
    if (j.contains("control_vector_layer_start")) {
        control_vector_settings_.control_vector_layer_start = j.value("control_vector_layer_start", 0);
    }
    if (j.contains("control_vector_layer_end")) {
        control_vector_settings_.control_vector_layer_end = j.value("control_vector_layer_end", -1);
    }
}

void Settings::deserializeTensorOverrideSettings(const json& j) {
    // Tensor overrides
    if (j.contains("tensor_overrides")) {
        tensor_override_settings_.overrides.clear();
        for (const auto& ov : j["tensor_overrides"]) {
            TensorOverrideSettings::TensorOverride override;
            override.pattern = ov.value("pattern", "");
            override.buffer_type = ov.value("buffer_type", "");
            tensor_override_settings_.overrides.push_back(override);
        }
    }
}

} // namespace core
} // namespace llama_gui
