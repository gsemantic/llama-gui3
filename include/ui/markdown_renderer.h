#pragma once

/*
 * markdown_renderer.h — Рендеринг markdown-подобного текста в ImGui
 * с подсветкой синтаксиса кода и визуальным разделением thinking/answer.
 */

#include "imgui.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace llama_gui {
namespace ui {

/* Цветовая тема подсветки синтаксиса. */
struct SyntaxTheme {
    ImVec4 keyword   = ImVec4(0.56f, 0.75f, 1.0f,  1.0f);  // синий
    ImVec4 string    = ImVec4(0.68f, 0.88f, 0.52f,  1.0f);  // зелёный
    ImVec4 comment   = ImVec4(0.50f, 0.50f, 0.50f,  1.0f);  // серый
    ImVec4 number    = ImVec4(1.0f,  0.65f, 0.40f,  1.0f);  // оранжевый
    ImVec4 preproc   = ImVec4(0.75f, 0.50f, 1.0f,   1.0f);  // фиолетовый
    ImVec4 type      = ImVec4(0.45f, 0.85f, 0.85f,  1.0f);  // циан
    ImVec4 function  = ImVec4(1.0f,  0.85f, 0.40f,  1.0f);  // жёлтый
    ImVec4 operator_ = ImVec4(0.90f, 0.65f, 0.75f,  1.0f);  // розовый
    ImVec4 bracket   = ImVec4(0.80f, 0.80f, 0.80f,  1.0f);  // светло-серый
    ImVec4 plain     = ImVec4(0.85f, 0.85f, 0.85f,  1.0f);  // обычный текст в блоке кода

    /* Цвета блока кода (фон, рамка). */
    ImVec4 code_bg       = ImVec4(0.12f, 0.12f, 0.16f, 1.0f);
    ImVec4 code_border   = ImVec4(0.25f, 0.25f, 0.35f, 1.0f);
    ImVec4 code_header   = ImVec4(0.40f, 0.40f, 0.50f, 1.0f);  // язык в заголовке

    /* Thinking-блок. */
    ImVec4 thinking_bg     = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
    ImVec4 thinking_border = ImVec4(0.30f, 0.25f, 0.40f, 1.0f);
    ImVec4 thinking_text   = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);
    ImVec4 thinking_label  = ImVec4(0.65f, 0.55f, 0.80f, 1.0f);

    /* Инлайн-код. */
    ImVec4 inline_code_bg = ImVec4(0.18f, 0.18f, 0.24f, 1.0f);
};

/* Тип токена подсветки. */
enum class TokKind {
    Plain, Keyword, String, Comment, Number, Preproc,
    Type, Function, Operator, Bracket
};

/* Токен подсветки: [start, end) в строке кода + тип. */
struct HighlightToken {
    size_t start;
    size_t end;
    TokKind kind;
};

/* Рендерер markdown-подобного текста с подсветкой кода. */
class MarkdownRenderer {
public:
    static MarkdownRenderer& instance();

    /* Основной метод: рендерит текст с подсветкой. */
    void render(const std::string& text, const ImVec4& base_color);

    /* Рендерит текст с поддержкой thinking-блоков.
     * Блоки <think>...</think> и <thinking>...</thinking> отображаются
     * свёрнутыми с отличающимимся стилем. */
    void render_with_thinking(const std::string& text, const ImVec4& base_color);

    /* Подсветка одной строки кода заданного языка.
     * x_offset — смещение от CursorScreenPos.x (для номеров строк). */
    void render_code_line(const std::string& line, const std::string& lang,
                          float x_offset = 0.0f);

    /* Получить текущую тему. */
    const SyntaxTheme& theme() const { return theme_; }
    void set_theme(const SyntaxTheme& t) { theme_ = t; }

    /* Включить/выключить подсветку. */
    void set_enabled(bool e) { enabled_ = e; }
    bool is_enabled() const { return enabled_; }

private:
    MarkdownRenderer();
    SyntaxTheme theme_;
    bool enabled_ = true;

    /* Парсинг текста на блоки: обычный текст / код / thinking. */
    struct Block {
        enum Kind { Text, Code, Thinking } kind;
        std::string content;
        std::string lang;       // для Code
        bool collapsed;         // для Thinking
    };
    std::vector<Block> parse_blocks(const std::string& text);

    /* Рендеринг обычного текста (с инлайн-кодом, **жирным**, *курсивом*). */
    void render_text_block(const std::string& text, const ImVec4& base_color);

    /* Рендеринг блока кода с подсветкой. */
    void render_code_block(const std::string& code, const std::string& lang);

    /* Рендеринг thinking-блока (свёрнутый/развёрнутый). */
    void render_thinking_block(const std::string& content);

    /* Лексер для подсветки — возвращает токены для строки. */
    std::vector<HighlightToken> tokenize(const std::string& line, const std::string& lang);

    /* Специфичные лексеры. */
    std::vector<HighlightToken> tokenize_cpp(const std::string& line);
    std::vector<HighlightToken> tokenize_python(const std::string& line);
    std::vector<HighlightToken> tokenize_json(const std::string& line);
    std::vector<HighlightToken> tokenize_bash(const std::string& line);
    std::vector<HighlightToken> tokenize_php(const std::string& line);
    std::vector<HighlightToken> tokenize_sql(const std::string& line);
    std::vector<HighlightToken> tokenize_xml_html(const std::string& line);

    /* Утилиты. */
    ImVec4 tok_color(TokKind kind) const;
    static bool is_word_char(char c);
    static std::string trim(const std::string& s);

    /* Состояние свёрнутости thinking-блоков (по индексу в сообщении). */
    std::unordered_map<int, bool> thinking_collapsed_;
    int thinking_counter_ = 0;
};

} // namespace ui
} // namespace llama_gui
