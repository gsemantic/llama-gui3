#include "../include/ui/settings_dialog_batch.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

BatchDialog::BatchDialog(Settings& settings)
    : settings_(settings) {}

BatchDialog::~BatchDialog() = default;

void BatchDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void BatchDialog::render_batch_sizes_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Batch Sizes");
    ImGui::Separator();

    auto& batch = settings_.batch();

    // Batch size
    {
        int size = batch.batch_size;
        if (ImGui::SliderInt("Batch Size (-b)##batch_size", &size, 1, 8192)) {
            batch.batch_size = size;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of tokens to process in one batch");
    }

    // Ubatch size
    {
        int size = batch.ubatch_size;
        if (ImGui::SliderInt("Ubatch Size (-ub)##ubatch_size", &size, 1, 2048)) {
            batch.ubatch_size = size;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Physical batch size (must be <= batch size)");
    }

    ImGui::Separator();

    // Context size
    {
        int ctx = batch.ctx_size;
        if (ImGui::SliderInt("Context Size (-c)##ctx_size", &ctx, 128, 131072)) {
            batch.ctx_size = ctx;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Context size in tokens");
    }

    // Draft context size
    {
        int ctx_draft = batch.ctx_size_draft;
        if (ImGui::SliderInt("Draft Context (-cd)##ctx_draft", &ctx_draft, 0, 16384)) {
            batch.ctx_size_draft = ctx_draft;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Context size for draft model (0 = auto)");
    }

    ImGui::Separator();

    // Continuous batching
    {
        bool cont = batch.cont_batching;
        if (ImGui::Checkbox("Continuous Batching (-cb)##cont_batch", &cont)) {
            batch.cont_batching = cont;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Enable continuous batching");
    }

    // No perf
    {
        bool no_perf = batch.no_perf;
        if (ImGui::Checkbox("Disable Perf Optimizations##no_perf", &no_perf)) {
            batch.no_perf = no_perf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Disable performance optimizations");
    }
}

void BatchDialog::render_threading_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Threading");
    ImGui::Separator();

    auto& batch = settings_.batch();

    // Threads
    {
        int threads = batch.threads;
        if (ImGui::SliderInt("Threads (-t)##threads", &threads, -1, 64)) {
            batch.threads = threads;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of CPU threads (-1 = auto)");
    }

    // Threads batch
    {
        int threads_batch = batch.threads_batch;
        if (ImGui::SliderInt("Threads Batch (-TB)##threads_batch", &threads_batch, -1, 64)) {
            batch.threads_batch = threads_batch;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Threads for batch processing (-1 = same as threads)");
    }

    ImGui::Separator();

    // Show if separate batch threads are used
    if (batch.uses_separate_batch_threads()) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "* Separate batch threads configured");
    }
}

void BatchDialog::render_cpu_affinity_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "CPU Affinity");
    ImGui::Separator();

    auto& batch = settings_.batch();

    ImGui::Text("Main Thread Affinity:");
    ImGui::Separator();

    // CPU Mask
    {
        char buf[256];
        strncpy(buf, batch.cpu_mask.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("CPU Mask (-C)##cpu_mask", buf, sizeof(buf))) {
            batch.cpu_mask = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("CPU mask (e.g., \"0-3\" or \"0,2,4,6\")");
    }

    // CPU Range
    {
        char buf[256];
        strncpy(buf, batch.cpu_range.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("CPU Range (-Cr)##cpu_range", buf, sizeof(buf))) {
            batch.cpu_range = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("CPU range (e.g., \"0-7\")");
    }

    // CPU Strict
    {
        bool strict = batch.cpu_strict;
        if (ImGui::Checkbox("CPU Strict##cpu_strict", &strict)) {
            batch.cpu_strict = strict;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Strict CPU affinity enforcement");
    }

    ImGui::Separator();

    // Priority
    {
        int prio = batch.priority;
        if (ImGui::SliderInt("Priority (--prio)##priority", &prio, -20, 20)) {
            batch.priority = prio;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Process priority (-20 to 20)");
    }

    // Poll level
    {
        int poll = batch.poll_level;
        if (ImGui::SliderInt("Poll Level (--poll)##poll", &poll, 0, 100)) {
            batch.poll_level = poll;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("CPU polling level (0-100)");
    }

    ImGui::Separator();

    // Batch CPU Affinity (collapsible)
    if (ImGui::CollapsingHeader("Batch Thread CPU Affinity")) {
        ImGui::Indent();

        // CPU Mask Batch
        {
            char buf[256];
            strncpy(buf, batch.cpu_mask_batch.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            if (ImGui::InputText("CPU Mask Batch (-Cb)##cpu_mask_batch", buf, sizeof(buf))) {
                batch.cpu_mask_batch = buf;
                modified_ = true;
            }
        }

        // CPU Range Batch
        {
            char buf[256];
            strncpy(buf, batch.cpu_range_batch.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            if (ImGui::InputText("CPU Range Batch (-Crb)##cpu_range_batch", buf, sizeof(buf))) {
                batch.cpu_range_batch = buf;
                modified_ = true;
            }
        }

        // CPU Strict Batch
        {
            bool strict = batch.cpu_strict_batch;
            if (ImGui::Checkbox("CPU Strict Batch##cpu_strict_batch", &strict)) {
                batch.cpu_strict_batch = strict;
                modified_ = true;
            }
        }

        // Priority Batch
        {
            int prio = batch.priority_batch;
            if (ImGui::SliderInt("Priority Batch (--prio-batch)##priority_batch", &prio, -20, 20)) {
                batch.priority_batch = prio;
                modified_ = true;
            }
        }

        // Poll Batch
        {
            int poll = batch.poll_batch;
            if (ImGui::SliderInt("Poll Batch (--poll-batch)##poll_batch", &poll, 0, 100)) {
                batch.poll_batch = poll;
                modified_ = true;
            }
        }

        ImGui::Unindent();
    }

    // Show if batch CPU affinity is configured
    if (batch.uses_batch_cpu_affinity()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "* Separate batch CPU affinity configured");
    }
}

void BatchDialog::render() {
    // =========================================================================
    // Batch Sizes Section
    // =========================================================================
    render_batch_sizes_section();

    ImGui::Separator();

    // =========================================================================
    // Threading Section
    // =========================================================================
    render_threading_section();

    ImGui::Separator();

    // =========================================================================
    // CPU Affinity Section
    // =========================================================================
    render_cpu_affinity_section();

    // =========================================================================
    // Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text("Configuration Summary:");

    auto& batch = settings_.batch();

    ImGui::BulletText("Batch: %d (ubatch: %d)", batch.batch_size, batch.ubatch_size);
    ImGui::BulletText("Context: %d (draft: %d)", batch.ctx_size, batch.ctx_size_draft);
    ImGui::BulletText("Threads: %d (batch: %s)",
                      batch.get_effective_threads(),
                      batch.uses_separate_batch_threads() ?
                          std::to_string(batch.get_effective_batch_threads()).c_str() : "same");

    if (batch.uses_batch_cpu_affinity()) {
        ImGui::BulletText("CPU Affinity: Separate batch configuration");
    } else if (!batch.cpu_mask.empty() || !batch.cpu_range.empty()) {
        ImGui::BulletText("CPU Affinity: %s%s",
                          !batch.cpu_mask.empty() ? batch.cpu_mask.c_str() : "",
                          !batch.cpu_range.empty() ? batch.cpu_range.c_str() : "");
    }

    ImGui::BulletText("Priority: %d (batch: %d)", batch.priority, batch.priority_batch);
}

} // namespace ui
} // namespace llama_gui
