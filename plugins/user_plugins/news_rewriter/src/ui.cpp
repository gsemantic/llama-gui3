#include "ui.h"

#include <cstdio>
#include <string>

#include "imgui.h"

#include "common.h"
#include "worker.h"

namespace news_rewriter {

namespace {

std::string format_duration(int seconds) {
    if (seconds < 0) return "-";
    if (seconds < 60) return std::to_string(seconds) + " с";
    return std::to_string(seconds / 60) + " мин";
}

// Текстовое поле для std::string (буфер ограничен, для длинных промптов
// используйте input_text_multiline).
void input_text(const char* label, std::string& value) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (ImGui::InputText(label, buf, sizeof(buf))) value = buf;
}

// Многострочное поле: название — отдельной строкой НАД полем. ImGui рисует
// label справа от виджета, а ширина -1.0f растягивает поле на всю ширину окна —
// длинное название уезжало бы за правую границу. Здесь label в строке сверху.
void input_text_multiline(const char* label, std::string& value, float height) {
    ImGui::TextWrapped("%s", label);
    char buf[16384];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (ImGui::InputTextMultiline(("##" + std::string(label)).c_str(),
                                  buf, sizeof(buf), ImVec2(-1.0f, height))) {
        value = buf;
    }
}

void combo_type(const char* label, std::string& type) {
    if (ImGui::BeginCombo(label, type.c_str())) {
        for (const char* t : {"rss", "atom", "page"}) {
            const bool selected = (type == t);
            if (ImGui::Selectable(t, selected)) type = t;
        }
        ImGui::EndCombo();
    }
}

// InputInt с запретом отрицательных значений (0 = выключено/без лимита).
void input_int_min0(const char* label, int& value) {
    if (ImGui::InputInt(label, &value) && value < 0) value = 0;
}

// Раздел настроек. draft — редактируемая копия конфигурации.
void render_settings(UiDeps& deps, Config& draft) {
    // ---- Источники (сайты для обхода) -------------------------------------
    ImGui::TextUnformatted("Источники (сайты для обхода):");
    int remove_index = -1;
    for (std::size_t i = 0; i < draft.sources.size(); i++) {
        SourceConfig& s = draft.sources[i];
        char label[64];
        std::snprintf(label, sizeof(label), "##src_en_%zu", i);
        ImGui::Checkbox(label, &s.enabled);
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "##src_type_%zu", i);
        ImGui::SetNextItemWidth(70.0f);
        combo_type(label, s.type);
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "##src_url_%zu", i);
        char url[2048];
        std::snprintf(url, sizeof(url), "%s", s.url.c_str());
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::InputText(label, url, sizeof(url))) s.url = url;
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), "Удалить##%zu", i);
        if (ImGui::Button(label)) remove_index = static_cast<int>(i);
    }
    if (remove_index >= 0) {
        draft.sources.erase(draft.sources.begin() + remove_index);
    }

    // Добавление нового источника.
    static std::string new_type = "rss";
    static std::string new_url;
    ImGui::SetNextItemWidth(70.0f);
    combo_type("##new_type", new_type);
    ImGui::SameLine();
    char new_url_buf[2048];
    std::snprintf(new_url_buf, sizeof(new_url_buf), "%s", new_url.c_str());
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::InputText("##new_url", new_url_buf, sizeof(new_url_buf))) {
        new_url = new_url_buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Добавить источник") && !new_url.empty()) {
        draft.sources.push_back(SourceConfig{new_url, new_type, SourceExtract{}, true});
        new_url.clear();
    }

    ImGui::Separator();

    // ---- Сбор --------------------------------------------------------------
    ImGui::TextUnformatted("Сбор:");
    input_int_min0("Интервал автозапуска, мин (0 = только вручную)",
                   draft.schedule_minutes);
    input_int_min0("Статей с источника (0 = без лимита)",
                   draft.max_items_per_source);
    input_int_min0("Свежесть, часов (0 = без ограничения)",
                   draft.max_age_hours);

    ImGui::Separator();

    // ---- Рерайт -------------------------------------------------------------
    ImGui::TextUnformatted("Рерайт:");
    input_text("Язык", draft.rewrite.language);
    input_text("Тон", draft.rewrite.tone);
    input_int_min0("Примерный объём статьи, слов (0 = без ограничения)",
                   draft.rewrite.max_words);
    input_text_multiline("Промпт ({title} {body} {language} {tone} {max_words})",
                         draft.rewrite.prompt_template, 140.0f);

    ImGui::Separator();

    // ---- Выход --------------------------------------------------------------
    ImGui::TextUnformatted("Выход:");
    if (ImGui::BeginCombo("Тип вывода", draft.sink.type.c_str())) {
        for (const char* t : {"local_file", "http"}) {
            const bool selected = (draft.sink.type == t);
            if (ImGui::Selectable(t, selected)) draft.sink.type = t;
        }
        ImGui::EndCombo();
    }
    input_text("Выходная папка (пусто = каталог данных приложения)",
               draft.sink.output_dir);
    if (draft.sink.type == "http") {
        std::string url = draft.sink.params["url"].as_string();
        input_text("URL приёмника", url);
        draft.sink.params["url"] = url;
        std::string api_key = draft.sink.params["api_key"].as_string();
        input_text("API-ключ", api_key);
        draft.sink.params["api_key"] = api_key;
        int timeout = static_cast<int>(draft.sink.params["timeout_seconds"].as_int(20));
        if (ImGui::InputInt("Таймаут отправки, с", &timeout) && timeout < 0) timeout = 0;
        draft.sink.params["timeout_seconds"] = timeout;
    }

    ImGui::Separator();

    // ---- Сеть ----------------------------------------------------------------
    ImGui::TextUnformatted("Сеть:");
    input_int_min0("Таймаут загрузки, с", draft.network.timeout_seconds);
    input_text("User-Agent", draft.network.user_agent);
    input_text("Прокси (пусто = системный)", draft.network.proxy);
    input_text_multiline("Доп. заголовки (Header: value на строку)",
                         draft.network.extra_headers, 60.0f);

    ImGui::Separator();

    // ---- Действия ------------------------------------------------------------
    static double saved_at = 0.0;   // время последнего нажатия «Сохранить»
    if (ImGui::Button("Сохранить")) {
        if (deps.on_save) deps.on_save(draft);
        saved_at = ImGui::GetTime();
    }
    ImGui::SameLine();
    if (ImGui::Button("Сбросить")) {
        if (deps.worker) draft = deps.worker->get_config();
    }
    if (saved_at > 0.0) {
        const double elapsed = ImGui::GetTime() - saved_at;
        if (elapsed < 5.0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Сохранено");
        } else {
            saved_at = 0.0;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Настройки сохраняются по кнопке «Сохранить» или при закрытии окна");
}

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

    // Крестик в заголовке (p_open): при закрытии скрываем окно в хосте и
    // сбрасываем флаг, чтобы повторное открытие через меню снова показало окно.
    static bool s_window_open = true;
    static bool s_reset_open = false;
    if (s_reset_open) {
        s_window_open = true;
        s_reset_open = false;
    }

    const WorkerState state = deps.worker->snapshot();
    const Config cfg = deps.worker->get_config();

    // Черновик настроек: инициализируется из конфига воркера при первом кадре,
    // дальше редактируется пользователем. «Сбросить» возвращает его из воркера.
    static Config draft;
    static bool draft_loaded = false;
    if (!draft_loaded) {
        draft = cfg;
        draft_loaded = true;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("News Rewriter", &s_window_open);
    if (!s_window_open) {
        // Крестик нажат — применяем черновик настроек (автосейв) и скрываем окно.
        ImGui::End();
        if (deps.on_save) deps.on_save(draft);
        if (deps.on_close) deps.on_close();
        s_reset_open = true;
        return;
    }
    if (!visible) {
        // Окно свёрнуто в заголовок — содержимое не рисуем, но не закрываем.
        ImGui::End();
        return;
    }

    // Прокручиваемое содержимое: контент (статус, настройки, задачи) выше
    // дефолтной высоты окна, без скролла нижние поля обрезались бы и не
    // принимали ввод (в т.ч. вставку из буфера обмена).
    ImGui::BeginChild("scroll", ImVec2(0, 0), false);

    ImGui::TextUnformatted("Агент: обход адресов, рерайт новостей через LLM, сохранение локально");
    ImGui::Separator();

    // Статус воркера
    ImGui::TextColored(state.running ? ImVec4(1.0f, 0.85f, 0.0f, 1.0f)
                                     : ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                       "Воркер: %s", state.running ? "обход идёт" : "простаивает");
    ImGui::Text("Поток: %s", state.worker_active ? "активен" : "остановлен");
    ImGui::Text("Ожидающих задач: %d", state.pending_tasks);
    if (state.error_count > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "Итог последнего обхода: обработано %d, ОШИБОК: %d",
                           state.done_count, state.error_count);
    } else if (state.done_count > 0) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                           "Итог последнего обхода: обработано %d, ошибок: %d",
                           state.done_count, state.error_count);
    }
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
    if (state.scheduled) {
        if (state.running) {
            ImGui::TextDisabled("расписание: каждые %d мин, следующий — после обхода",
                                cfg.schedule_minutes);
        } else if (state.next_run_in_seconds <= 0) {
            ImGui::TextDisabled("расписание: каждые %d мин, запуск сейчас",
                                cfg.schedule_minutes);
        } else {
            ImGui::TextDisabled("расписание: каждые %d мин, следующий через %s",
                                cfg.schedule_minutes,
                                format_duration(state.next_run_in_seconds).c_str());
        }
    } else {
        ImGui::TextDisabled("расписание выключено (только ручной запуск)");
    }

    ImGui::Separator();

    // Настройки
    if (ImGui::CollapsingHeader("Настройки", ImGuiTreeNodeFlags_DefaultOpen)) {
        render_settings(deps, draft);
        ImGui::Separator();
    }

    // Статусы задач
    if (ImGui::CollapsingHeader("Задачи", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace news_rewriter
