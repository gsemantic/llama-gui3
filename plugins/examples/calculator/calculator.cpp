/*
 * calculator.cpp — плагин «Калькулятор выражений» для llama-gui.
 *
 * Демонстрирует:
 *   - окно плагина (window_register + Dear ImGui в ll_plugin_render);
 *   - меню и команды (menu_add / command_register);
 *   - диалоги (dialog_info);
 *   - LLM (llm_complete — блокирующий запрос);
 *   - настройки/состояние (settings_set / state_get).
 *
 * Выражение считается локально рекурсивным спуском (без eval).
 */

#include "plugins/plugin_api.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static LlamaPluginHost* g_host = nullptr;
static const LlamaHostApi* g_api = nullptr;
static LlamaPluginWindow* g_window = nullptr;

// ============================================================================
// Локальный парсер выражений (рекурсивный спуск)
// ============================================================================

namespace {

class Parser {
public:
    explicit Parser(const std::string& expr) : expr_(expr) {}

    bool parse(double& result) {
        try {
            size_t p = skip_ws(0);
            result = expr(p);
            p = skip_ws(p);
            if (p != expr_.size()) {
                error_ = "неожиданный символ: '" + std::string(1, expr_[p]) + "'";
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            error_ = e.what();
            return false;
        }
    }

    const std::string& error() const { return error_; }

private:
    const std::string& expr_;
    std::string error_;
    size_t pos_ = 0;

    size_t skip_ws(size_t p) {
        while (p < expr_.size() && (expr_[p] == ' ' || expr_[p] == '\t')) p++;
        return p;
    }

    double expr(size_t& p) {
        double value = term(p);
        for (;;) {
            p = skip_ws(p);
            if (p < expr_.size() && expr_[p] == '+') { p++; value += term(p); }
            else if (p < expr_.size() && expr_[p] == '-') { p++; value -= term(p); }
            else break;
        }
        return value;
    }

    double term(size_t& p) {
        double value = factor(p);
        for (;;) {
            p = skip_ws(p);
            if (p < expr_.size() && expr_[p] == '*') { p++; value *= factor(p); }
            else if (p < expr_.size() && expr_[p] == '/') {
                p++;
                double divisor = factor(p);
                if (divisor == 0.0) throw std::runtime_error("деление на ноль");
                value /= divisor;
            }
            else break;
        }
        return value;
    }

    double factor(size_t& p) {
        p = skip_ws(p);
        if (p >= expr_.size()) throw std::runtime_error("неожиданный конец выражения");

        if (expr_[p] == '+') { p++; return factor(p); }
        if (expr_[p] == '-') { p++; return -factor(p); }

        if (expr_[p] == '(') {
            p++;
            double v = expr(p);
            p = skip_ws(p);
            if (p >= expr_.size() || expr_[p] != ')')
                throw std::runtime_error("нет закрывающей скобки");
            p++;
            return v;
        }

        if (expr_[p] >= '0' && expr_[p] <= '9') {
            size_t start = p;
            while (p < expr_.size() &&
                   ((expr_[p] >= '0' && expr_[p] <= '9') ||
                    expr_[p] == '.' || expr_[p] == 'e' || expr_[p] == 'E' ||
                    ((expr_[p] == '+' || expr_[p] == '-') && p > start &&
                     (expr_[p - 1] == 'e' || expr_[p - 1] == 'E')))) {
                p++;
            }
            return std::stod(expr_.substr(start, p - start));
        }

        throw std::runtime_error("ожидалось число");
    }
};

std::string format_result(double value) {
    if (value == static_cast<long long>(value)) {
        return std::to_string(static_cast<long long>(value));
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", value);
    return buf;
}

} // namespace

// ============================================================================
// Команды плагина
// ============================================================================

static void cmd_open_window(void*) {
    if (g_api && g_host && g_window) {
        g_api->window_set_visible(g_host, g_window, 1);
    }
}

static void cmd_about(void*) {
    if (g_api && g_host) {
        g_api->dialog_info(g_host, "Calculator Plugin",
                           "Плагин-калькулятор: вычисляет выражение локально "
                           "или через LLM-сервер.");
    }
}

// ============================================================================
// Экспортируемые функции плагина
// ============================================================================

extern "C" {

LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void) {
    return LLAMA_PLUGIN_API_VERSION;
}

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "calculator",
        "1.0.0",
        "Калькулятор выражений: локальный расчёт + LLM",
        "llama-gui"
    };
    return &info;
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    if (!host || !api) return 1;
    g_host = host;
    g_api = api;

    if (g_api->log) {
        g_api->log(g_host, LLAMA_LOG_INFO, "calculator: инициализация");
    }

    g_api->command_register(g_host, "calculator_open", cmd_open_window, nullptr,
                            "Open Calculator window", "Ctrl+Shift+C");
    g_api->command_register(g_host, "calculator_about", cmd_about, nullptr,
                            "About Calculator", nullptr);

    LlamaPluginMenu* menu = g_api->menu_add(g_host, "Calculator");
    if (menu) {
        g_api->menu_add_item(g_host, menu, "Open Calculator", "calculator_open", "Ctrl+Shift+C");
        g_api->menu_add_item(g_host, menu, "About", "calculator_about", nullptr);
    }

    g_window = g_api->window_register(g_host, "calculator", "Calculator");

    // Состояние: запоминаем последнее выражение между запусками (в памяти)
    return 0;
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api || !g_host || !g_window) return;
    if (g_api->window_is_visible(g_host, g_window) != 1) return;

    static char input[512] = "2 + 2 * 2";
    static std::string result;
    static std::string error;
    static bool use_llm = false;

    ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
    ImGui::Begin("Calculator");

    ImGui::Text("Введите математическое выражение:");
    ImGui::InputText("##expr", input, sizeof(input));
    ImGui::Checkbox("Считать через LLM", &use_llm);
    ImGui::SameLine();
    ImGui::TextDisabled("(блокирующий запрос)");

    if (ImGui::Button("=", ImVec2(120, 0))) {
        result.clear();
        error.clear();

        if (use_llm) {
            char* response = nullptr;
            const std::string prompt =
                "Вычисли и ответь только числом: " + std::string(input);
            if (g_api->llm_complete(g_host, prompt.c_str(), &response) == 1 && response) {
                result = response;
                g_api->free_string(g_host, response);
            } else {
                error = "LLM-сервер и облако недоступны или не ответили";
            }
        } else {
            double value = 0.0;
            Parser parser(input);
            if (parser.parse(value)) {
                result = format_result(value);
                // Запоминаем результат в state (in-memory) и в settings (persist)
                g_api->state_set(g_host, "calculator.last_result", result.c_str());
                g_api->settings_set(g_host, "calculator.last_expression",
                                    ("{\"expr\":\"" + std::string(input) + "\"}").c_str());
            } else {
                error = parser.error();
            }
        }
    }

    ImGui::Separator();

    if (!error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Ошибка: %s", error.c_str());
    }
    if (!result.empty()) {
        ImGui::Text("Результат: %s", result.c_str());
    }

    ImGui::Separator();
    const int connected = g_api->llm_is_connected(g_host);
    ImGui::TextColored(connected ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                 : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "LLM-сервер: %s%s",
                       connected ? "подключен" : "не подключен",
                       connected ? "" : " (при запросе используется облако)");

    ImGui::End();
}

LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void) {
    if (g_api && g_host) {
        g_api->log(g_host, LLAMA_LOG_INFO, "calculator: выгрузка");
    }
    g_host = nullptr;
    g_api = nullptr;
    g_window = nullptr;
}

} // extern "C"
