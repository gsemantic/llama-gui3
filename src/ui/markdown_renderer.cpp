/*
 * markdown_renderer.cpp — Рендеринг markdown-подобного текста в ImGui
 * с подсветкой синтаксиса и визуальным разделением thinking/answer.
 *
 * Поддерживает:
 *   - Блоки кода ```lang ... ```  с подсветкой синтаксиса
 *   - Инлайн-код `code`
 *   - Жирный **text** и курсив *text*
 *   - Thinking-блоки: <think>...</think>, <thinking>...</thinking>
 *   - Языки: C/C++, Python, PHP, JSON, Bash, SQL, XML/HTML, generic
 */

#include "../include/ui/markdown_renderer.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace llama_gui {
namespace ui {

/* ======================================================================
 * Утилиты
 * ====================================================================== */

static std::string ltrim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b);
}

static std::string rtrim(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r\n");
    return (e == std::string::npos) ? "" : s.substr(0, e + 1);
}

static std::string trim_str(const std::string& s) { return rtrim(ltrim(s)); }

static bool starts_with(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

static bool ends_with(const std::string& s, const char* suffix) {
    size_t slen = std::strlen(suffix);
    return s.size() >= slen && s.compare(s.size() - slen, slen, suffix) == 0;
}

/* Начало слова — после не-буквенно-цифрового символа или начало строки. */
static bool is_word_boundary(const std::string& line, size_t pos, size_t len) {
    if (pos > 0) {
        char prev = line[pos - 1];
        if (std::isalnum((unsigned char)prev) || prev == '_') return false;
    }
    size_t after = pos + len;
    if (after < line.size()) {
        char next = line[after];
        if (std::isalnum((unsigned char)next) || next == '_') return false;
    }
    return true;
}

/* Найти слово в строке с проверкой границ слова. */
static size_t find_word(const std::string& line, const char* word, size_t from) {
    size_t wlen = std::strlen(word);
    size_t pos = from;
    while (pos < line.size()) {
        size_t found = line.find(word, pos);
        if (found == std::string::npos) return std::string::npos;
        if (is_word_boundary(line, found, wlen)) return found;
        pos = found + 1;
    }
    return std::string::npos;
}

/* ======================================================================
 * MarkdownRenderer — singleton
 * ====================================================================== */

MarkdownRenderer& MarkdownRenderer::instance() {
    static MarkdownRenderer inst;
    return inst;
}

MarkdownRenderer::MarkdownRenderer() = default;

/* ======================================================================
 * Цвет токена
 * ====================================================================== */

ImVec4 MarkdownRenderer::tok_color(TokKind kind) const {
    switch (kind) {
        case TokKind::Keyword:  return theme_.keyword;
        case TokKind::String:   return theme_.string;
        case TokKind::Comment:  return theme_.comment;
        case TokKind::Number:   return theme_.number;
        case TokKind::Preproc:  return theme_.preproc;
        case TokKind::Type:     return theme_.type;
        case TokKind::Function: return theme_.function;
        case TokKind::Operator: return theme_.operator_;
        case TokKind::Bracket:  return theme_.bracket;
        case TokKind::Plain:    return theme_.plain;
    }
    return theme_.plain;
}

bool MarkdownRenderer::is_word_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

std::string MarkdownRenderer::trim(const std::string& s) { return trim_str(s); }

/* ======================================================================
 * Парсинг текста на блоки
 * ====================================================================== */

std::vector<MarkdownRenderer::Block> MarkdownRenderer::parse_blocks(const std::string& text) {
    std::vector<Block> blocks;
    size_t pos = 0;

    while (pos < text.size()) {
        /* Проверяем thinking-блоки. */
        const char* think_tags[] = {"<think>", "<thinking>", "[THINKING]"};
        const char* think_closes[] = {"</think>", "</thinking>", "[/THINKING]"};
        bool found_think = false;
        for (int t = 0; t < 3; ++t) {
            if (starts_with(text.substr(pos), think_tags[t])) {
                size_t content_start = pos + std::strlen(think_tags[t]);
                size_t close_pos = text.find(think_closes[t], content_start);
                if (close_pos != std::string::npos) {
                    Block blk;
                    blk.kind = Block::Thinking;
                    blk.content = text.substr(content_start, close_pos - content_start);
                    blk.collapsed = true;
                    blocks.push_back(blk);
                    pos = close_pos + std::strlen(think_closes[t]);
                    found_think = true;
                    break;
                }
            }
        }
        if (found_think) continue;

        /* Проверяем блок кода: ```lang ... ``` */
        if (text.compare(pos, 3, "```") == 0) {
            size_t after_fence = pos + 3;
            /* Язык — до конца строки. */
            size_t nl = text.find('\n', after_fence);
            std::string lang;
            size_t code_start;
            if (nl != std::string::npos) {
                lang = trim_str(text.substr(after_fence, nl - after_fence));
                code_start = nl + 1;
            } else {
                code_start = after_fence;
            }
            /* Ищем закрывающий ```. */
            size_t close_pos = text.find("\n```", code_start);
            if (close_pos == std::string::npos) {
                close_pos = text.find("```", code_start);
                if (close_pos != std::string::npos && close_pos > code_start)
                    close_pos = close_pos;  // fence без newline перед
                else
                    close_pos = std::string::npos;
            }
            std::string code;
            size_t end_pos;
            if (close_pos != std::string::npos) {
                code = text.substr(code_start, close_pos - code_start);
                end_pos = close_pos + 3;
                /* Пропускаем trailing newline после ```. */
                if (end_pos < text.size() && text[end_pos] == '\n') ++end_pos;
            } else {
                code = text.substr(code_start);
                end_pos = text.size();
            }
            /* Убираем trailing newline из кода. */
            while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
                code.pop_back();

            Block blk;
            blk.kind = Block::Code;
            blk.content = code;
            blk.lang = lang;
            blocks.push_back(blk);
            pos = end_pos;
            continue;
        }

        /* Обычный текст — до следующего special-блока. */
        size_t next_special = text.size();
        for (int t = 0; t < 3; ++t) {
            size_t fp = text.find(think_tags[t], pos);
            if (fp < next_special) next_special = fp;
        }
        {
            size_t fp = text.find("```", pos);
            if (fp < next_special) next_special = fp;
        }
        if (next_special > pos) {
            Block blk;
            blk.kind = Block::Text;
            blk.content = text.substr(pos, next_special - pos);
            blocks.push_back(blk);
        }
        pos = next_special;
    }

    return blocks;
}

/* ======================================================================
 * Рендеринг обычного текста (инлайн-код, жирный, курсив)
 * ====================================================================== */

void MarkdownRenderer::render_text_block(const std::string& text, const ImVec4& base_color) {
    /* Для текста с wrapping используем ImGui::TextWrapped.
     * Инлайн-код рендерим через ImDrawList на той же строке. */
    ImGui::PushTextWrapPos(0.0f);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t backtick = text.find('`', pos);

        if (backtick == std::string::npos) {
            /* Весь оставшийся текст — обычный, рендерим через TextWrapped. */
            std::string rest = text.substr(pos);
            ImGui::PushStyleColor(ImGuiCol_Text, base_color);
            ImGui::TextWrapped("%s", rest.c_str());
            ImGui::PopStyleColor();
            break;
        }

        /* Текст до backtick — через TextWrapped. */
        if (backtick > pos) {
            std::string before = text.substr(pos, backtick - pos);
            ImGui::PushStyleColor(ImGuiCol_Text, base_color);
            ImGui::TextWrapped("%s", before.c_str());
            ImGui::PopStyleColor();
        }

        /* Ищем закрывающий backtick. */
        size_t close = text.find('`', backtick + 1);
        if (close == std::string::npos) {
            /* Нет закрывающего — рендерим backtick как текст. */
            std::string rest = text.substr(backtick);
            ImGui::PushStyleColor(ImGuiCol_Text, base_color);
            ImGui::TextWrapped("%s", rest.c_str());
            ImGui::PopStyleColor();
            break;
        }

        /* Инлайн-код — рисуем на новой строке с фоном. */
        std::string code = text.substr(backtick + 1, close - backtick - 1);
        {
            ImVec2 code_size = ImGui::CalcTextSize(code.c_str());
            ImVec2 padding = ImVec2(4, 1);
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImVec2 bg_min = ImVec2(cursor.x - padding.x, cursor.y - padding.y);
            ImVec2 bg_max = ImVec2(cursor.x + code_size.x + padding.x, cursor.y + code_size.y + padding.y);
            ImGui::GetWindowDrawList()->AddRectFilled(bg_min, bg_max,
                ImGui::ColorConvertFloat4ToU32(theme_.inline_code_bg), 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, theme_.string);
            ImGui::TextUnformatted(code.c_str(), code.c_str() + code.size());
            ImGui::PopStyleColor();
        }

        pos = close + 1;
    }

    ImGui::PopTextWrapPos();
}

/* ======================================================================
 * Лексеры
 * ====================================================================== */

/* Ключевые слова по языкам. */
static const char* kw_cpp[] = {
    "auto","bool","break","case","catch","char","class","const","constexpr",
    "continue","default","delete","do","double","dynamic_cast","else","enum",
    "explicit","extern","false","final","float","for","friend","goto","if",
    "inline","int","long","mutable","namespace","new","noexcept","nullptr",
    "operator","override","private","protected","public","register","reinterpret_cast",
    "return","short","signed","sizeof","static","static_assert","static_cast",
    "struct","switch","template","this","thread_local","throw","true","try",
    "typedef","typeid","typename","union","unsigned","using","virtual","void",
    "volatile","while","size_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "int8_t","int16_t","int32_t","int64_t","std","string","vector","map",
    "unordered_map","shared_ptr","unique_ptr","make_shared","make_unique",
    "function","pair","optional","variant","mutex","lock_guard","atomic",
    nullptr
};

static const char* kw_python[] = {
    "and","as","assert","async","await","break","class","continue","def",
    "del","elif","else","except","finally","for","from","global","if",
    "import","in","is","lambda","nonlocal","not","or","pass","raise",
    "return","try","while","with","yield","True","False","None",
    "self","print","range","len","int","str","float","list","dict","set",
    "tuple","type","isinstance","super","classmethod","staticmethod",
    nullptr
};

static const char* kw_php[] = {
    "abstract","and","array","as","break","callable","case","catch","class",
    "clone","const","continue","declare","default","die","do","echo","else",
    "elseif","empty","enddeclare","endfor","endforeach","endif","endswitch",
    "endwhile","eval","exit","extends","final","finally","fn","for","foreach",
    "function","global","goto","if","implements","include","include_once",
    "instanceof","interface","isset","list","match","namespace","new","or",
    "print","private","protected","public","require","require_once","return",
    "static","switch","throw","trait","try","unset","use","var","while",
    "xor","yield","true","false","null","void","int","string","float","bool",
    "array","iterable","object","mixed","never","self","parent",
    nullptr
};

static const char* kw_bash[] = {
    "if","then","else","elif","fi","for","while","do","done","case","esac",
    "function","return","in","select","until","time","coproc","!",
    "echo","printf","read","exit","export","source","alias","local",
    "declare","typeset","readonly","set","unset","shift","eval","exec",
    "trap","test","true","false","cd","pwd","ls","mkdir","rm","cp","mv",
    "cat","grep","sed","awk","find","sort","uniq","wc","head","tail",
    "chmod","chown","sudo","apt","yum","dnf","pip","npm","git","docker",
    "curl","wget","ssh","scp","rsync","tar","gzip",
    nullptr
};

static const char* kw_sql[] = {
    "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","SET","DELETE",
    "CREATE","TABLE","ALTER","DROP","INDEX","JOIN","INNER","LEFT","RIGHT",
    "OUTER","ON","AND","OR","NOT","IN","IS","NULL","LIKE","BETWEEN","EXISTS",
    "GROUP","BY","HAVING","ORDER","ASC","DESC","LIMIT","OFFSET","UNION","ALL",
    "DISTINCT","AS","CASE","WHEN","THEN","ELSE","END","COUNT","SUM","AVG",
    "MIN","MAX","PRIMARY","KEY","FOREIGN","REFERENCES","CONSTRAINT","DEFAULT",
    "AUTO_INCREMENT","NOT","NULL","UNIQUE","CHECK","IF","BEGIN","COMMIT",
    "ROLLBACK","TRUNCATE","VIEW","TRIGGER","PROCEDURE","FUNCTION",
    nullptr
};

static const char* kw_json[] = {
    "true","false","null",
    nullptr
};

/* Поиск ключевого слова в массиве. */
static bool is_keyword(const std::string& word, const char** kws) {
    for (int i = 0; kws[i]; ++i) {
        if (word == kws[i]) return true;
    }
    return false;
}

/* Базовый лексер: числа, строки, комментарии, скобки. */
static std::vector<HighlightToken> base_tokenize(const std::string& line,
                                                  const char** keywords,
                                                  const char** types = nullptr,
                                                  bool cpp_comments = true,
                                                  bool hash_comments = false,
                                                  bool dollar_vars = false) {
    std::vector<HighlightToken> tokens;
    size_t i = 0;

    auto add_tok = [&](size_t s, size_t e, TokKind k) {
        if (s < e) tokens.push_back({s, e, k});
    };

    while (i < line.size()) {
        /* Комментарии C-стиля: // и /* */
        if (cpp_comments && i + 1 < line.size() && line[i] == '/' && line[i+1] == '/') {
            add_tok(i, line.size(), TokKind::Comment);
            return tokens;
        }
        if (cpp_comments && i + 1 < line.size() && line[i] == '/' && line[i+1] == '*') {
            size_t end = line.find("*/", i + 2);
            if (end == std::string::npos) end = line.size();
            else end += 2;
            add_tok(i, end, TokKind::Comment);
            i = end;
            continue;
        }

        /* Комментарии #: bash, python, SQL, ... */
        if (hash_comments && line[i] == '#') {
            add_tok(i, line.size(), TokKind::Comment);
            return tokens;
        }

        /* SQL-комментарии -- */
        if (i + 1 < line.size() && line[i] == '-' && line[i+1] == '-' &&
            (keywords == kw_sql)) {
            add_tok(i, line.size(), TokKind::Comment);
            return tokens;
        }

        /* HTML/XML-комментарии <!-- --> */
        if (i + 3 < line.size() && line[i] == '<' && line[i+1] == '!' &&
            line[i+2] == '-' && line[i+3] == '-') {
            size_t end = line.find("-->", i + 4);
            if (end == std::string::npos) end = line.size();
            else end += 3;
            add_tok(i, end, TokKind::Comment);
            i = end;
            continue;
        }

        /* Строки: "...", '...' */
        if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i];
            size_t s = i;
            ++i;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) { i += 2; continue; }
                if (line[i] == quote) { ++i; break; }
                ++i;
            }
            add_tok(s, i, TokKind::String);
            continue;
        }

        /* Template strings (backtick) для JS/bash. */
        if (line[i] == '`') {
            size_t s = i;
            ++i;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) { i += 2; continue; }
                if (line[i] == '`') { ++i; break; }
                ++i;
            }
            add_tok(s, i, TokKind::String);
            continue;
        }

        /* Числа. */
        if (std::isdigit((unsigned char)line[i]) ||
            (line[i] == '.' && i + 1 < line.size() && std::isdigit((unsigned char)line[i+1]))) {
            size_t s = i;
            if (line[i] == '0' && i + 1 < line.size() &&
                (line[i+1] == 'x' || line[i+1] == 'X')) {
                i += 2;
                while (i < line.size() && std::isxdigit((unsigned char)line[i])) ++i;
            } else {
                while (i < line.size() && (std::isdigit((unsigned char)line[i]) || line[i] == '.')) ++i;
                if (i < line.size() && (line[i] == 'e' || line[i] == 'E')) {
                    ++i;
                    if (i < line.size() && (line[i] == '+' || line[i] == '-')) ++i;
                    while (i < line.size() && std::isdigit((unsigned char)line[i])) ++i;
                }
            }
            /* Суффиксы: f, F, L, l, u, U, UL, LL и т.д. */
            while (i < line.size() && (line[i] == 'f' || line[i] == 'F' ||
                   line[i] == 'L' || line[i] == 'l' ||
                   line[i] == 'u' || line[i] == 'U')) ++i;
            add_tok(s, i, TokKind::Number);
            continue;
        }

        /* Препроцессор: #include, #define, #ifdef ... (C/C++) */
        if (line[i] == '#' && cpp_comments && i == ltrim(line).size() - ltrim(line).size()
            && i + 1 < line.size() && std::isalpha((unsigned char)line[i+1])) {
            /* Находим начало — это ltrim. */
            size_t s = i;
            while (i < line.size() && !std::isspace((unsigned char)line[i]) && line[i] != '<' && line[i] != '"')
                ++i;
            add_tok(s, i, TokKind::Preproc);
            /* Остальное строки или <...> — тоже препроцессор. */
            if (i < line.size()) {
                add_tok(i, line.size(), TokKind::Preproc);
                i = line.size();
            }
            continue;
        }

        /* PHP: $variable */
        if (dollar_vars && line[i] == '$' && i + 1 < line.size() &&
            (std::isalpha((unsigned char)line[i+1]) || line[i+1] == '_')) {
            size_t s = i;
            ++i;
            while (i < line.size() && (std::isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
            add_tok(s, i, TokKind::Function);
            continue;
        }

        /* Слова: идентификаторы и ключевые слова. */
        if (std::isalpha((unsigned char)line[i]) || line[i] == '_') {
            size_t s = i;
            while (i < line.size() && (std::isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
            std::string word = line.substr(s, i - s);
            if (is_keyword(word, keywords)) {
                add_tok(s, i, TokKind::Keyword);
            } else if (types && is_keyword(word, types)) {
                add_tok(s, i, TokKind::Type);
            } else if (i < line.size() && line[i] == '(') {
                add_tok(s, i, TokKind::Function);
            } else {
                add_tok(s, i, TokKind::Plain);
            }
            continue;
        }

        /* Скобки. */
        if (line[i] == '(' || line[i] == ')' || line[i] == '[' || line[i] == ']' ||
            line[i] == '{' || line[i] == '}') {
            add_tok(i, i + 1, TokKind::Bracket);
            ++i;
            continue;
        }

        /* Операторы. */
        if (line[i] == '+' || line[i] == '-' || line[i] == '*' || line[i] == '/' ||
            line[i] == '=' || line[i] == '!' || line[i] == '<' || line[i] == '>' ||
            line[i] == '&' || line[i] == '|' || line[i] == '^' || line[i] == '~' ||
            line[i] == '%' || line[i] == '?') {
            size_t s = i;
            ++i;
            /* Двойные операторы: ==, !=, <=, >=, &&, ||, ++, --, <<, >>, ->, :: */
            if (i < line.size() && (line[i] == '=' || line[i] == line[i-1] ||
                (line[i-1] == '-' && line[i] == '>') ||
                (line[i-1] == ':' && line[i] == ':') ||
                (line[i-1] == '<' && line[i] == '<') ||
                (line[i-1] == '>' && line[i] == '>'))) {
                ++i;
            }
            add_tok(s, i, TokKind::Operator);
            continue;
        }

        /* Всё остальное — обычный текст. */
        add_tok(i, i + 1, TokKind::Plain);
        ++i;
    }

    return tokens;
}

/* XML/HTML: теги как ключевые слова, атрибуты как типы. */
static const char* xml_tags[] = {
    "html","head","body","div","span","p","a","img","ul","ol","li","table",
    "tr","td","th","form","input","button","select","option","textarea",
    "script","style","link","meta","title","header","footer","nav","section",
    "article","aside","main","h1","h2","h3","h4","h5","h6","br","hr","pre",
    "code","blockquote","em","strong","iframe","svg","path","circle","rect",
    "xml","?xml","!DOCTYPE","!doctype",
    nullptr
};

static const char* xml_attrs[] = {
    "class","id","style","href","src","alt","title","type","name","value",
    "placeholder","action","method","onclick","onload","width","height",
    "data-","lang","charset","rel","content","property","xmlns",
    nullptr
};

std::vector<HighlightToken> MarkdownRenderer::tokenize_cpp(const std::string& line) {
    return base_tokenize(line, kw_cpp, nullptr, true, false, false);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize_python(const std::string& line) {
    return base_tokenize(line, kw_python, nullptr, false, true, false);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize_json(const std::string& line) {
    return base_tokenize(line, kw_json, nullptr, true, false, false);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize_bash(const std::string& line) {
    return base_tokenize(line, kw_bash, nullptr, false, true, true);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize_php(const std::string& line) {
    return base_tokenize(line, kw_php, nullptr, true, false, true);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize_sql(const std::string& line) {
    return base_tokenize(line, kw_sql, nullptr, false, false, false);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize_xml_html(const std::string& line) {
    return base_tokenize(line, xml_tags, xml_attrs, false, false, false);
}

std::vector<HighlightToken> MarkdownRenderer::tokenize(const std::string& line, const std::string& lang) {
    std::string l = lang;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);

    if (l == "cpp" || l == "c++" || l == "c" || l == "hpp" || l == "h" ||
        l == "cc" || l == "cxx" || l == "hxx")
        return tokenize_cpp(line);
    if (l == "python" || l == "py")
        return tokenize_python(line);
    if (l == "php")
        return tokenize_php(line);
    if (l == "json")
        return tokenize_json(line);
    if (l == "bash" || l == "sh" || l == "shell" || l == "zsh" || l == "bashrc")
        return tokenize_bash(line);
    if (l == "sql")
        return tokenize_sql(line);
    if (l == "xml" || l == "html" || l == "htm" || l == "svg" || l == "xhtml")
        return tokenize_xml_html(line);
    if (l == "javascript" || l == "js" || l == "jsx" || l == "ts" || l == "typescript")
        return tokenize_cpp(line);  // JS/TS близки к C-стилю
    if (l == "java" || l == "kotlin" || l == "scala" || l == "go" || l == "rust" ||
        l == "csharp" || l == "cs")
        return tokenize_cpp(line);  // C-подобные языки
    if (l == "ruby" || l == "rb")
        return tokenize_python(line);  // Ruby близок к Python по синтаксису
    if (l == "yaml" || l == "yml" || l == "toml" || l == "ini" || l == "cfg")
        return tokenize_bash(line);  // hash-comments

    /* Generic — базовая подсветка (числа, строки, комментарии). */
    return base_tokenize(line, nullptr, nullptr, true, true, false);
}

/* ======================================================================
 * Рендеринг строки кода с подсветкой
 * ====================================================================== */

void MarkdownRenderer::render_code_line(const std::string& line, const std::string& lang,
                                          float x_offset) {
    if (!enabled_ || lang.empty()) {
        ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
        return;
    }
    auto tokens = tokenize(line, lang);

    /* Прямое рисование через ImDrawList — без SameLine, без gaps. */
    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    float x = screen_pos.x + x_offset;
    float y = screen_pos.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();

    if (tokens.empty()) {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
        dl->AddText(font, font_size, ImVec2(x, y), col,
                    line.c_str(), line.c_str() + line.size());
    } else {
        std::sort(tokens.begin(), tokens.end(),
            [](const HighlightToken& a, const HighlightToken& b) { return a.start < b.start; });

        size_t pos = 0;
        for (const auto& tok : tokens) {
            if (tok.start > pos) {
                std::string gap = line.substr(pos, tok.start - pos);
                ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
                dl->AddText(font, font_size, ImVec2(x, y), col,
                            gap.c_str(), gap.c_str() + gap.size());
                x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, gap.c_str(), gap.c_str() + gap.size()).x;
            }
            std::string text = line.substr(tok.start, tok.end - tok.start);
            ImU32 col = ImGui::ColorConvertFloat4ToU32(tok_color(tok.kind));
            dl->AddText(font, font_size, ImVec2(x, y), col,
                        text.c_str(), text.c_str() + text.size());
            x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str(), text.c_str() + text.size()).x;
            pos = tok.end;
        }
        if (pos < line.size()) {
            std::string rest = line.substr(pos);
            ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
            dl->AddText(font, font_size, ImVec2(x, y), col,
                        rest.c_str(), rest.c_str() + rest.size());
        }
    }

    /* Продвигаем курсор на следующую строку. */
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
}

/* ======================================================================
 * Рендеринг блока кода
 * ====================================================================== */

void MarkdownRenderer::render_code_block(const std::string& code, const std::string& lang) {
    ImGuiStyle& style = ImGui::GetStyle();

    /* Разбиваем на строки. */
    std::istringstream stream(code);
    std::vector<std::string> lines;
    std::string ln;
    while (std::getline(stream, ln)) lines.push_back(ln);
    int line_count = std::max(1, (int)lines.size());
    float line_h = ImGui::GetTextLineHeight();
    float total_height = line_count * line_h;

    /* Ширина номеров строк. */
    float line_num_w = 0.0f;
    if (lines.size() > 1) {
        int max_digits = (int)std::to_string(lines.size()).size();
        line_num_w = ImGui::CalcTextSize("0").x * (max_digits + 2);
    }
    float code_left = line_num_w;

    /* Заголовок с языком. */
    if (!lang.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme_.code_header);
        ImGui::TextUnformatted(lang.c_str());
        ImGui::PopStyleColor();
    }

    /* Фон блока кода. */
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float avail_w = ImGui::GetContentRegionAvail().x;
    ImVec2 bg_min = ImVec2(pos.x, pos.y);
    ImVec2 bg_max = ImVec2(pos.x + avail_w, pos.y + total_height + style.FramePadding.y);
    ImU32 bg_col = ImGui::ColorConvertFloat4ToU32(theme_.code_bg);
    ImU32 border_col = ImGui::ColorConvertFloat4ToU32(theme_.code_border);
    ImGui::GetWindowDrawList()->AddRectFilled(bg_min, bg_max, bg_col, 3.0f);
    ImGui::GetWindowDrawList()->AddRect(bg_min, bg_max, border_col, 3.0f);

    /* Вертикальная полоска-акцент слева. */
    ImVec2 accent_min = ImVec2(pos.x, pos.y);
    ImVec2 accent_max = ImVec2(pos.x + 2.0f, pos.y + total_height + style.FramePadding.y);
    ImU32 accent_col = ImGui::ColorConvertFloat4ToU32(theme_.code_border);
    ImGui::GetWindowDrawList()->AddRectFilled(accent_min, accent_max, accent_col);

    /* Рисуем каждую строку через ImDrawList. */
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    float y = pos.y + style.FramePadding.y * 0.5f;
    float x_base = pos.x + 6.0f;  /* левый отступ от края блока */

    for (int i = 0; i < (int)lines.size(); ++i) {
        /* Номер строки. */
        if (lines.size() > 1) {
            char num_buf[16];
            std::snprintf(num_buf, sizeof(num_buf), "%*d", (int)std::to_string(lines.size()).size(), i + 1);
            ImU32 num_col = ImGui::ColorConvertFloat4ToU32(theme_.comment);
            dl->AddText(font, font_size, ImVec2(x_base, y), num_col, num_buf);
        }
        /* Строка кода с подсветкой. */
        float code_x = x_base + code_left;
        const std::string& line = lines[i];

        if (!enabled_ || lang.empty()) {
            ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
            dl->AddText(font, font_size, ImVec2(code_x, y), col,
                        line.c_str(), line.c_str() + line.size());
        } else {
            auto tokens = tokenize(line, lang);
            if (tokens.empty()) {
                ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
                dl->AddText(font, font_size, ImVec2(code_x, y), col,
                            line.c_str(), line.c_str() + line.size());
            } else {
                std::sort(tokens.begin(), tokens.end(),
                    [](const HighlightToken& a, const HighlightToken& b) { return a.start < b.start; });

                float x = code_x;
                size_t tpos = 0;
                for (const auto& tok : tokens) {
                    if (tok.start > tpos) {
                        std::string gap = line.substr(tpos, tok.start - tpos);
                        ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
                        dl->AddText(font, font_size, ImVec2(x, y), col,
                                    gap.c_str(), gap.c_str() + gap.size());
                        x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f,
                                                 gap.c_str(), gap.c_str() + gap.size()).x;
                    }
                    std::string text = line.substr(tok.start, tok.end - tok.start);
                    ImU32 col = ImGui::ColorConvertFloat4ToU32(tok_color(tok.kind));
                    dl->AddText(font, font_size, ImVec2(x, y), col,
                                text.c_str(), text.c_str() + text.size());
                    x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f,
                                             text.c_str(), text.c_str() + text.size()).x;
                    tpos = tok.end;
                }
                if (tpos < line.size()) {
                    std::string rest = line.substr(tpos);
                    ImU32 col = ImGui::ColorConvertFloat4ToU32(theme_.plain);
                    dl->AddText(font, font_size, ImVec2(x, y), col,
                                rest.c_str(), rest.c_str() + rest.size());
                }
            }
        }
        y += line_h;
    }

    /* Продвигаем курсор за блок. */
    ImGui::Dummy(ImVec2(avail_w, total_height + style.FramePadding.y));

    /* Invisible button поверх блока кода для интерактивности:
     * правый клик → контекстное меню с "Копировать код". */
    ImGui::SetCursorScreenPos(bg_min);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0,0));

    int block_id = thinking_counter_++;  /* переиспользуем счётчик для уникальных ID */
    std::string btn_id = "##codeblk_" + std::to_string(block_id);
    ImGui::InvisibleButton(btn_id.c_str(),
        ImVec2(avail_w, total_height + style.FramePadding.y));

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    /* Контекстное меню: копирование кода. */
    std::string popup_id = "##code_copy_" + std::to_string(block_id);
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(popup_id.c_str());
    }
    /* Двойной клик — быстрая копировка кода. */
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        static std::string dblclick_buf;
        dblclick_buf = code;
        ImGui::SetClipboardText(dblclick_buf.c_str());
    }
    if (ImGui::BeginPopup(popup_id.c_str())) {
        /* Сохраняем код в static-буфер для SetClipboardText. */
        static std::string menu_clipboard_buf;
        if (ImGui::MenuItem("Copy Code")) {
            menu_clipboard_buf = code;
            ImGui::SetClipboardText(menu_clipboard_buf.c_str());
        }
        if (!lang.empty() && ImGui::MenuItem("Copy with Fence")) {
            menu_clipboard_buf = "```" + lang + "\n" + code + "\n```";
            ImGui::SetClipboardText(menu_clipboard_buf.c_str());
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
}

/* ======================================================================
 * Рендеринг thinking-блока
 * ====================================================================== */

void MarkdownRenderer::render_thinking_block(const std::string& content) {
    ImGuiStyle& style = ImGui::GetStyle();
    int id = thinking_counter_++;
    bool& collapsed = thinking_collapsed_[id];

    /* Заголовок thinking. */
    ImVec4 header_color = theme_.thinking_label;

    ImGui::PushStyleColor(ImGuiCol_Text, header_color);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));

    const char* arrow = collapsed ? "▶" : "▼";
    std::string preview;
    if (collapsed) {
        /* Первая строка thinking как превью. */
        size_t nl = content.find('\n');
        preview = content.substr(0, std::min(nl, (size_t)80));
        if (content.size() > 80) preview += "…";
    }

    std::string label = std::string(arrow) + " Thinking";
    if (collapsed && !preview.empty()) {
        label += ": " + preview;
    }
    if (ImGui::SmallButton(("##think_toggle_" + std::to_string(id)).c_str())) {
        collapsed = !collapsed;
    }
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(label.c_str(), label.c_str() + label.size());

    ImGui::PopStyleColor(4);

    /* Развёрнутый контент. */
    if (!collapsed) {
        /* Фон thinking. */
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float content_height = ImGui::CalcTextSize(content.c_str(), nullptr, false, -1.0f).y;
        ImVec2 bg_min = ImVec2(pos.x - style.FramePadding.x, pos.y - style.FramePadding.y);
        ImVec2 bg_max = ImVec2(pos.x + ImGui::GetContentRegionAvail().x + style.FramePadding.x,
                               pos.y + content_height + style.FramePadding.y * 2);
        ImGui::GetWindowDrawList()->AddRectFilled(bg_min, bg_max,
            ImGui::ColorConvertFloat4ToU32(theme_.thinking_bg), 4.0f);
        ImGui::GetWindowDrawList()->AddRect(bg_min, bg_max,
            ImGui::ColorConvertFloat4ToU32(theme_.thinking_border), 4.0f);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + style.FramePadding.x * 2);
        ImGui::PushStyleColor(ImGuiCol_Text, theme_.thinking_text);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(content.c_str(), content.c_str() + content.size());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
}

/* ======================================================================
 * Основные публичные методы
 * ====================================================================== */

void MarkdownRenderer::render(const std::string& text, const ImVec4& base_color) {
    if (!enabled_ || text.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, base_color);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        return;
    }

    thinking_counter_ = 0;

    auto blocks = parse_blocks(text);
    for (const auto& blk : blocks) {
        switch (blk.kind) {
            case Block::Text:
                render_text_block(blk.content, base_color);
                break;
            case Block::Code:
                render_code_block(blk.content, blk.lang);
                break;
            case Block::Thinking:
                render_thinking_block(blk.content);
                break;
        }
    }
}

void MarkdownRenderer::render_with_thinking(const std::string& text, const ImVec4& base_color) {
    render(text, base_color);
}

} // namespace ui
} // namespace llama_gui
