#include "../include/ui/settings_dialog_context.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

ContextDialog::ContextDialog(Settings& settings)
    : settings_(settings) {}

ContextDialog::~ContextDialog() = default;

void ContextDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

const char* ContextDialog::get_cache_type_label(llama_gui::core::CacheSettings::CacheType type) {
    switch (type) {
        case llama_gui::core::CacheSettings::CacheType::F32:    return TR("cache.f32");
        case llama_gui::core::CacheSettings::CacheType::F16:    return TR("cache.f16");
        case llama_gui::core::CacheSettings::CacheType::BF16:   return TR("cache.bf16");
        case llama_gui::core::CacheSettings::CacheType::Q8_0:   return TR("cache.q8_0");
        case llama_gui::core::CacheSettings::CacheType::Q4_0:   return TR("cache.q4_0");
        case llama_gui::core::CacheSettings::CacheType::Q4_1:   return TR("cache.q4_1");
        case llama_gui::core::CacheSettings::CacheType::IQ4_NL: return TR("cache.iq4_nl");
        case llama_gui::core::CacheSettings::CacheType::Q5_0:   return TR("cache.q5_0");
        case llama_gui::core::CacheSettings::CacheType::Q5_1:   return TR("cache.q5_1");
        default: return TR("cache.f16");
    }
}

void ContextDialog::render() {
    auto& cache = settings_.cache();
    auto& batch = settings_.batch();

    // =========================================================================
    // Context Size Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("context.size"));
    ImGui::Separator();

    // Main context size
    {
        int ctx_size = batch.ctx_size;
        if (ImGui::SliderInt(TR("context.size"), &ctx_size, 128, 131072)) {
            batch.ctx_size = ctx_size;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.size.help"));

        // Показываем рекомендацию для max_tokens при изменении ctx_size
        int recommended = settings_.get_recommended_max_tokens();
        int max_allowed = settings_.get_max_allowed_tokens();
        int reserve = settings_.get_prompt_reserve();

        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), 
            "Рекомендуемый максимум генерации: ~%d токенов", recommended);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::Text("При контексте %d токенов:", ctx_size);
            ImGui::BulletText("Запас на промпт/историю (20%%): %d токенов", reserve);
            ImGui::BulletText("Максимум для генерации: %d токенов", max_allowed);
            ImGui::BulletText("Рекомендуется (25%%): ~%d токенов", recommended);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // Draft context size
    {
        int ctx_draft = batch.ctx_size_draft;
        if (ImGui::SliderInt(TR("context.draft_size"), &ctx_draft, 0, 16384)) {
            batch.ctx_size_draft = ctx_draft;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.draft_size.help"));
    }

    // N_predict
    {
        auto& output = settings_.output();
        int n_predict = output.n_predict;
        
        // Визуальная индикация синхронизации с chat.max_tokens
        auto& chat = settings_.chat();
        bool is_synced = (n_predict == chat.max_tokens);
        
        if (is_synced && n_predict != -1) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), TR("context.max_predict"));
            ImGui::SameLine();
            ImGui::TextDisabled("(sync)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Значение синхронизировано с chat.max_tokens (%d)", chat.max_tokens);
                ImGui::Text("Синхронизация происходит при загрузке профиля");
                ImGui::EndTooltip();
            }
        } else {
            ImGui::Text(TR("context.max_predict"));
            ImGui::SameLine();
        }
        
        if (ImGui::SliderInt("##max_predict", &n_predict, -1, 32768)) {
            output.n_predict = n_predict;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.max_predict.help"));
    }

    // Keep tokens
    {
        auto& output = settings_.output();
        int keep = output.keep;
        if (ImGui::SliderInt(TR("context.keep"), &keep, 0, 8192)) {
            output.keep = keep;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.keep.help"));
    }

    ImGui::Separator();

    // =========================================================================
    // KV Cache Types Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "KV Cache Types");
    ImGui::Separator();

    ImGui::Text(TR("context.cache_type_k"));
    ImGui::SameLine();

    {
        int current = static_cast<int>(cache.cache_type_k);
        const char* cache_types[] = {
            TR("cache.f32"), TR("cache.f16"), TR("cache.bf16"),
            TR("cache.q8_0"), TR("cache.q4_0"), TR("cache.q4_1"),
            TR("cache.iq4_nl"), TR("cache.q5_0"), TR("cache.q5_1")
        };
        if (ImGui::Combo("##cache_type_k", &current, cache_types, IM_ARRAYSIZE(cache_types))) {
            cache.cache_type_k = static_cast<llama_gui::core::CacheSettings::CacheType>(current);
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.cache_type_k.help"));
    }

    ImGui::Text(TR("context.cache_type_v"));
    ImGui::SameLine();

    {
        int current = static_cast<int>(cache.cache_type_v);
        const char* cache_types[] = {
            TR("cache.f32"), TR("cache.f16"), TR("cache.bf16"),
            TR("cache.q8_0"), TR("cache.q4_0"), TR("cache.q4_1"),
            TR("cache.iq4_nl"), TR("cache.q5_0"), TR("cache.q5_1")
        };
        if (ImGui::Combo("##cache_type_v", &current, cache_types, IM_ARRAYSIZE(cache_types))) {
            cache.cache_type_v = static_cast<llama_gui::core::CacheSettings::CacheType>(current);
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.cache_type_v.help"));
    }

    ImGui::Separator();

    // Draft cache types
    if (ImGui::CollapsingHeader("Draft Model Cache")) {
        ImGui::Indent();

        ImGui::Text(TR("context.draft_cache_k"));
        ImGui::SameLine();
        {
            int current = static_cast<int>(cache.cache_type_k_draft);
            const char* cache_types[] = {
                TR("cache.f32"), TR("cache.f16"), TR("cache.bf16"),
                TR("cache.q8_0"), TR("cache.q4_0"), TR("cache.q4_1"),
                TR("cache.iq4_nl"), TR("cache.q5_0"), TR("cache.q5_1")
            };
            if (ImGui::Combo("##cache_type_k_draft", &current, cache_types, IM_ARRAYSIZE(cache_types))) {
                cache.cache_type_k_draft = static_cast<llama_gui::core::CacheSettings::CacheType>(current);
                modified_ = true;
            }
        }

        ImGui::Text(TR("context.draft_cache_v"));
        ImGui::SameLine();
        {
            int current = static_cast<int>(cache.cache_type_v_draft);
            const char* cache_types[] = {
                TR("cache.f32"), TR("cache.f16"), TR("cache.bf16"),
                TR("cache.q8_0"), TR("cache.q4_0"), TR("cache.q4_1"),
                TR("cache.iq4_nl"), TR("cache.q5_0"), TR("cache.q5_1")
            };
            if (ImGui::Combo("##cache_type_v_draft", &current, cache_types, IM_ARRAYSIZE(cache_types))) {
                cache.cache_type_v_draft = static_cast<llama_gui::core::CacheSettings::CacheType>(current);
                modified_ = true;
            }
        }

        ImGui::Unindent();
    }

    ImGui::Separator();

    // =========================================================================
    // Cache Options Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Cache Options");
    ImGui::Separator();

    // Cache prompt
    {
        bool cache_prompt = cache.cache_prompt;
        if (ImGui::Checkbox(TR("context.cache_prompt"), &cache_prompt)) {
            cache.cache_prompt = cache_prompt;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.cache_prompt.help"));
    }

    // Cache reuse
    {
        int reuse = cache.cache_reuse;
        if (ImGui::SliderInt(TR("context.cache_reuse"), &reuse, 0, 100)) {
            cache.cache_reuse = reuse;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.cache_reuse.help"));
    }

    ImGui::Separator();

    // SWA full
    {
        bool swa_full = cache.swa_full;
        if (ImGui::Checkbox(TR("context.swa_full"), &swa_full)) {
            cache.swa_full = swa_full;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.swa_full.help"));
    }

    // No context shift
    {
        bool no_shift = cache.no_context_shift;
        if (ImGui::Checkbox(TR("context.no_shift"), &no_shift)) {
            cache.no_context_shift = no_shift;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.no_shift.help"));
    }

    ImGui::Separator();

    // =========================================================================
    // Slot Management Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Slot Management");
    ImGui::Separator();

    // Slot save path
    {
        char buf[512];
        strncpy(buf, cache.slot_save_path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(TR("context.slot_path"), buf, sizeof(buf))) {
            cache.slot_save_path = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.slot_path.help"));
    }

    // Slot prompt similarity
    {
        float sim = cache.slot_prompt_similarity;
        if (ImGui::SliderFloat(TR("context.slot_similarity"), &sim, 0.0f, 1.0f, "%.2f")) {
            cache.slot_prompt_similarity = sim;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.slot_similarity.help"));
    }

    // Slots endpoint enabled
    {
        bool enabled = cache.slots_endpoint_enabled;
        if (ImGui::Checkbox(TR("context.slot_endpoint"), &enabled)) {
            cache.slots_endpoint_enabled = enabled;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("context.slot_endpoint.help"));
    }

    // =========================================================================
    // Memory Estimation
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Memory Estimation")) {
        ImGui::Indent();

        // Rough estimation
        int ctx = batch.ctx_size;
        float mem_per_token = 0.0f;

        // Estimate based on cache type
        switch (cache.cache_type_k) {
            case llama_gui::core::CacheSettings::CacheType::F32: mem_per_token += 4.0f; break;
            case llama_gui::core::CacheSettings::CacheType::F16:
            case llama_gui::core::CacheSettings::CacheType::BF16: mem_per_token += 2.0f; break;
            case llama_gui::core::CacheSettings::CacheType::Q8_0: mem_per_token += 1.0f; break;
            case llama_gui::core::CacheSettings::CacheType::Q4_0:
            case llama_gui::core::CacheSettings::CacheType::Q4_1:
            case llama_gui::core::CacheSettings::CacheType::IQ4_NL: mem_per_token += 0.5f; break;
            case llama_gui::core::CacheSettings::CacheType::Q5_0:
            case llama_gui::core::CacheSettings::CacheType::Q5_1: mem_per_token += 0.625f; break;
        }

        switch (cache.cache_type_v) {
            case llama_gui::core::CacheSettings::CacheType::F32: mem_per_token += 4.0f; break;
            case llama_gui::core::CacheSettings::CacheType::F16:
            case llama_gui::core::CacheSettings::CacheType::BF16: mem_per_token += 2.0f; break;
            case llama_gui::core::CacheSettings::CacheType::Q8_0: mem_per_token += 1.0f; break;
            case llama_gui::core::CacheSettings::CacheType::Q4_0:
            case llama_gui::core::CacheSettings::CacheType::Q4_1:
            case llama_gui::core::CacheSettings::CacheType::IQ4_NL: mem_per_token += 0.5f; break;
            case llama_gui::core::CacheSettings::CacheType::Q5_0:
            case llama_gui::core::CacheSettings::CacheType::Q5_1: mem_per_token += 0.625f; break;
        }

        // Assume model dimension (this is rough estimate)
        float model_dim = 4096.0f; // Typical value
        float kv_cache_mb = (ctx * mem_per_token * model_dim) / (1024.0f * 1024.0f);

        ImGui::BulletText("Context Size: %d tokens", ctx);
        ImGui::BulletText("Estimated KV Cache: ~%.1f MB", kv_cache_mb);
        ImGui::BulletText("Note: Actual memory depends on model architecture");

        ImGui::Unindent();
    }
}

} // namespace ui
} // namespace llama_gui
