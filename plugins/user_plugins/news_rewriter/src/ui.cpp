#include "ui.h"

#include <cstdio>

#include "imgui.h"

#include "common.h"
#include "config.h"
#include "worker.h"

namespace news_rewriter {

namespace {

const ImVec4 status_color_task(TaskStatus s) {
    switch (s) {
        case TaskStatus::Done:   return ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        case TaskStatus::Error:  return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        case TaskStatus::Pending:
        case TaskStatus::Fetching:
        case TaskStatus::Extracting:
        case TaskStatus::Rewriting:
        case TaskStatus::Exporting: return ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

void render_news_rewriter_window(UiDeps& deps) {
    if (!deps.worker) return;

    const WorkerState state = deps.worker->snapshot();
    const Config cfg = deps.worker->get_config();

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("News Rewriter");

    ImGui::TextUnformatted("Агент: обход адресов, рерайт новостей через LLM, сохранение локально");
    ImGui::Separator();

    // Статус воркера
    ImGui::TextColored(state.running ? ImVec4(1.0f, 0.85f, 0.0f, 1.0f)
                                     : ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                       "Воркер: %s", state.running ? "обход идёт" : "простаивает");
    ImGui::Text("Поток: %s", state.worker_active ? "активен" : "остановлен");
    ImGui::Text("Ожидающих задач: %d", state.pending_tasks);
    ImGui::TextWrapped("Последнее: %s", state.last_message.c_str());

    if (state.running) {
        if (ImGui::Button("Остановить обход")) {
            deps.worker->post(Command{CmdType::Stop});
        }
    } else {
        if (ImGui::Button("Обойти сейчас")) {
            deps.worker->post(Command{CmdType::RunNow});
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("расписание: каждые %d мин", cfg.schedule_minutes);

    ImGui::Separator();

    // Источники (этап 0: только просмотр; редактирование — позже)
    ImGui::Text("Источники (%d):", static_cast<int>(cfg.sources.size()));
    for (const auto& s : cfg.sources) {
        ImGui::BulletText("[%s] %-6s %s",
                          s.enabled ? "on" : "off",
                          s.type.c_str(),
                          s.url.c_str());
    }

    ImGui::Separator();

    // Статусы задач
    ImGui::Text("Задачи:");
    if (state.articles.empty()) {
        ImGui::TextDisabled("Нет задач. Нажмите «Обойти сейчас».");
    }
    for (const auto& a : state.articles) {
        const ImVec4 color = status_color_task(a.status);
        ImGui::TextColored(color, "%-10s %s", task_status_name(a.status), a.source.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", a.url.c_str());
        if (!a.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  %s", a.error.c_str());
        }
    }

    ImGui::End();
}

} // namespace news_rewriter
