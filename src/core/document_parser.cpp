#include "../include/core/document_parser.h"
#include "../include/core/ast_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#ifdef USE_LIBZIP
#include <zip.h>
#endif

namespace llama_gui {
namespace core {

// Вспомогательная функция для проверки существования файла
static bool file_exists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

// Вспомогательная функция для извлечения текста из DOCX через unzip
static std::string extract_docx_text_via_unzip(const std::string& file_path) {
    std::string extracted_text;
    
    // Создаем временную директорию для распаковки
    std::string temp_dir = "/tmp/llama_gui_docx_" + std::to_string(getpid());
    std::string mkdir_cmd = "mkdir -p " + temp_dir + " 2>/dev/null";
    int mkdir_result = std::system(mkdir_cmd.c_str());
    (void)mkdir_result; // Игнорируем результат, если директория уже существует
    
    // Распаковываем document.xml из DOCX
    std::string unzip_cmd = "unzip -p \"" + file_path + "\" word/document.xml > " + temp_dir + "/document.xml 2>/dev/null";
    int unzip_result = std::system(unzip_cmd.c_str());
    
    if (unzip_result == 0) {
        // Читаем XML и извлекаем текст
        std::ifstream xml_file(temp_dir + "/document.xml");
        if (xml_file.is_open()) {
            std::stringstream buffer;
            buffer << xml_file.rdbuf();
            std::string xml_content = buffer.str();
            
            // Шаг 1: Удаляем все XML-теги и атрибуты, оставляем только текст
            std::string text_only;
            bool in_tag = false;
            bool in_script_or_style = false;
            
            for (size_t i = 0; i < xml_content.length(); ++i) {
                char c = xml_content[i];
                
                // Проверяем начало тега
                if (c == '<') {
                    in_tag = true;
                    
                    // Проверяем, не скрипт ли это или стиль
                    if (i + 7 < xml_content.length() && 
                        xml_content.substr(i, 7) == "<w:script") {
                        in_script_or_style = true;
                    }
                    if (i + 7 < xml_content.length() && 
                        xml_content.substr(i, 6) == "<w:style") {
                        in_script_or_style = true;
                    }
                    continue;
                }
                
                // Проверяем конец тега
                if (c == '>' && in_tag) {
                    in_tag = false;
                    
                    // Проверяем закрывающий тег скрипта или стиля
                    if (i >= 9 && xml_content.substr(i - 9, 9) == "</w:script>") {
                        in_script_or_style = false;
                    }
                    if (i >= 8 && xml_content.substr(i - 8, 8) == "</w:style>") {
                        in_script_or_style = false;
                    }
                    continue;
                }
                
                // Добавляем текст только если не в теге и не в скрипте/стиле
                if (!in_tag && !in_script_or_style) {
                    // Пропускаем управляющие символы XML
                    if (c == '&' && i + 4 < xml_content.length()) {
                        // Обрабатываем XML-entities
                        if (xml_content.substr(i, 5) == "&amp;") {
                            text_only += '&';
                            i += 4;
                            continue;
                        }
                        if (xml_content.substr(i, 4) == "&lt;") {
                            text_only += '<';
                            i += 3;
                            continue;
                        }
                        if (xml_content.substr(i, 4) == "&gt;") {
                            text_only += '>';
                            i += 3;
                            continue;
                        }
                        if (xml_content.substr(i, 6) == "&quot;") {
                            text_only += '"';
                            i += 5;
                            continue;
                        }
                        if (xml_content.substr(i, 6) == "&apos;") {
                            text_only += '\'';
                            i += 5;
                            continue;
                        }
                        if (xml_content.substr(i, 6) == "&nbsp;") {
                            text_only += ' ';
                            i += 5;
                            continue;
                        }
                    }
                    text_only += c;
                }
            }
            
            // Шаг 2: Очищаем от лишних пробелов и переносов
            std::string cleaned_text;
            bool last_was_space = true; // Начинаем с true, чтобы убрать лидирующие пробелы
            
            for (size_t i = 0; i < text_only.length(); ++i) {
                char c = text_only[i];
                
                // Заменяем табы и переносы на пробелы
                if (c == '\t' || c == '\n' || c == '\r') {
                    c = ' ';
                }
                
                // Убираем повторяющиеся пробелы
                if (c == ' ') {
                    if (!last_was_space) {
                        cleaned_text += c;
                        last_was_space = true;
                    }
                } else {
                    cleaned_text += c;
                    last_was_space = false;
                }
            }
            
            // Шаг 3: Разбиваем на предложения по точкам (для лучшей читаемости)
            extracted_text = cleaned_text;
            
            xml_file.close();
        }
        
        // Удаляем временные файлы
        std::string rm_cmd = "rm -rf " + temp_dir + " 2>/dev/null";
        int rm_result = std::system(rm_cmd.c_str());
        (void)rm_result;
    }
    
    return extracted_text;
}

std::vector<std::string> DocumentParser::parse_txt(const std::string& file_path) {
    std::vector<std::string> paragraphs;
    
    // Проверяем, что путь к файлу не пуст
    if (file_path.empty()) {
        std::cerr << "Error: Empty file path provided for TXT parsing" << std::endl;
        return paragraphs;
    }

    std::ifstream file(file_path);
    
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << file_path << std::endl;
        return paragraphs;
    }
    
    // Проверяем, что файл не пуст
    if (file.peek() == std::ifstream::traits_type::eof()) {
        std::cerr << "Warning: File is empty: " << file_path << std::endl;
        file.close();
        return paragraphs;
    }
    
    std::string line;
    std::string paragraph;
    
    while (std::getline(file, line)) {
        // Проверяем, что строка не содержит недопустимых символов
        if (line.find('\0') != std::string::npos) {
            std::cerr << "Warning: Null character found in line, skipping: " << file_path << std::endl;
            continue;
        }
        
        if (line.empty()) {
            if (!paragraph.empty()) {
                paragraphs.push_back(paragraph);
                paragraph.clear();
            }
        } else {
            if (!paragraph.empty()) {
                paragraph += "\n";
            }
            paragraph += line;
        }
    }
    
    // Добавляем последний параграф, если он не пуст
    if (!paragraph.empty()) {
        paragraphs.push_back(paragraph);
    }
    
    file.close();
    return paragraphs;
}

std::vector<std::string> DocumentParser::parse_pdf(const std::string& file_path) {
    std::vector<std::string> paragraphs;
    
    // Заглушка для парсинга PDF
    // В реальной реализации здесь будет интеграция с библиотекой для работы с PDF
    std::cerr << "Warning: PDF parsing not implemented yet. Using placeholder for " << file_path << std::endl;
    
    // Возвращаем фиктивный контент для демонстрации
    paragraphs.push_back("PDF content would be extracted here. This is a placeholder for " + file_path);
    
    return paragraphs;
}

std::vector<std::string> DocumentParser::parse_docx(const std::string& file_path) {
    std::vector<std::string> paragraphs;

    // Проверяем, что путь к файлу не пуст
    if (file_path.empty()) {
        std::cerr << "Error: Empty file path provided for DOCX parsing" << std::endl;
        return paragraphs;
    }

    // Проверяем существование файла
    std::ifstream file(file_path);
    if (!file.good()) {
        std::cerr << "Error: DOCX file does not exist or is not accessible: " << file_path << std::endl;
        return paragraphs;
    }
    file.close();

    std::cout << "Parsing DOCX file: " << file_path << std::endl;

    // Пытаемся извлечь текст через unzip
    std::string extracted_text = extract_docx_text_via_unzip(file_path);

    if (extracted_text.empty()) {
        std::cerr << "Warning: Could not extract text from DOCX file: " << file_path << std::endl;
        // Возвращаем placeholder для демонстрации
        paragraphs.push_back("DOCX file: " + file_path + "\n[Text extraction failed - try using 'unzip' command]");
        return paragraphs;
    }

    std::cout << "Successfully extracted " << extracted_text.length() << " characters from DOCX" << std::endl;

    // Разбиваем текст на параграфы по пустым строкам
    std::string current_paragraph;
    for (size_t i = 0; i < extracted_text.length(); ++i) {
        char c = extracted_text[i];

        // Обработка переносов строк
        if (c == '\n' || c == '\r') {
            if (!current_paragraph.empty()) {
                paragraphs.push_back(current_paragraph);
                current_paragraph.clear();
            }
        } else {
            current_paragraph += c;
        }
    }

    // Добавляем последний параграф
    if (!current_paragraph.empty()) {
        paragraphs.push_back(current_paragraph);
    }

    // Если параграфов нет, но текст есть - добавляем весь текст как один параграф
    if (paragraphs.empty() && !extracted_text.empty()) {
        paragraphs.push_back(extracted_text);
    }

    return paragraphs;
}

// Вспомогательная функция для очистки Markdown-разметки
static std::string clean_markdown(const std::string& markdown_text) {
    std::string cleaned;
    cleaned.reserve(markdown_text.length());

    std::string line;
    bool in_code_block = false;
    bool in_html_tag = false;

    for (size_t i = 0; i < markdown_text.length(); ++i) {
        char c = markdown_text[i];

        // Обработка переносов строк
        if (c == '\n') {
            // Проверка на code block (```)
            if (line.find("```") == 0) {
                in_code_block = !in_code_block;
            }

            if (!in_code_block && !line.empty()) {
                // Удаляем заголовки (#, ##, ### и т.д.)
                size_t hash_count = 0;
                while (hash_count < line.length() && line[hash_count] == '#') {
                    hash_count++;
                }

                if (hash_count > 0) {
                    // Это заголовок - пропускаем # и пробелы после них
                    size_t content_start = hash_count;
                    while (content_start < line.length() && line[content_start] == ' ') {
                        content_start++;
                    }
                    line = line.substr(content_start);
                }

                // Удаляем маркеры списков (-, *, +, 1.)
                if (!line.empty()) {
                    // Нумерованные списки (1., 2. и т.д.)
                    size_t digit_count = 0;
                    while (digit_count < line.length() && line[digit_count] >= '0' && line[digit_count] <= '9') {
                        digit_count++;
                    }
                    if (digit_count > 0 && digit_count < line.length() && line[digit_count] == '.') {
                        size_t content_start = digit_count + 1;
                        while (content_start < line.length() && line[content_start] == ' ') {
                            content_start++;
                        }
                        line = line.substr(content_start);
                    }
                    // Маркированные списки (-, *, +)
                    else if (line[0] == '-' || line[0] == '*' || line[0] == '+') {
                        size_t content_start = 1;
                        while (content_start < line.length() && line[content_start] == ' ') {
                            content_start++;
                        }
                        line = line.substr(content_start);
                    }
                }

                // Удаляем цитаты (>)
                if (!line.empty() && line[0] == '>') {
                    size_t content_start = 1;
                    while (content_start < line.length() && line[content_start] == ' ') {
                        content_start++;
                    }
                    line = line.substr(content_start);
                }

                cleaned += line;
            }
            cleaned += '\n';
            line.clear();
            in_html_tag = false;
            continue;
        }

        // Пропускаем содержимое code block
        if (in_code_block) {
            continue;
        }

        // Пропускаем HTML-теги
        if (c == '<') {
            in_html_tag = true;
            continue;
        }
        if (c == '>' && in_html_tag) {
            in_html_tag = false;
            continue;
        }
        if (in_html_tag) {
            continue;
        }

        // Пропускаем ссылки [text](url) - оставляем только текст
        if (c == '[') {
            // Ищем закрывающую ]
            size_t end_bracket = markdown_text.find(']', i + 1);
            if (end_bracket != std::string::npos && end_bracket + 1 < markdown_text.length() &&
                markdown_text[end_bracket + 1] == '(') {
                // Это ссылка - извлекаем текст между [ и ]
                size_t url_start = end_bracket + 2;
                size_t url_end = markdown_text.find(')', url_start);
                if (url_end != std::string::npos) {
                    // Добавляем только текст ссылки
                    cleaned += markdown_text.substr(i + 1, end_bracket - i - 1);
                    i = url_end;
                    continue;
                }
            }
        }

        // Пропускаем inline-код (`code`)
        if (c == '`') {
            // Ищем закрывающий `
            size_t end_code = markdown_text.find('`', i + 1);
            if (end_code != std::string::npos) {
                // Добавляем содержимое кода
                cleaned += markdown_text.substr(i + 1, end_code - i - 1);
                i = end_code;
                continue;
            }
        }

        // Пропускаем **жирный** и *курсив*
        if (c == '*' || c == '_') {
            // Проверяем на ** или __
            if (i + 1 < markdown_text.length() && markdown_text[i + 1] == c) {
                // Пропускаем открывающий **
                i += 2;
                // Ищем закрывающий **
                size_t end_bold = markdown_text.find(std::string(2, c), i);
                if (end_bold != std::string::npos) {
                    // Добавляем текст между **
                    cleaned += markdown_text.substr(i, end_bold - i);
                    i = end_bold + 1;
                    continue;
                }
            } else {
                // Одиночный * или _ - курсив
                i++;
                // Ищем закрывающий
                size_t end_italic = markdown_text.find(c, i);
                if (end_italic != std::string::npos) {
                    // Добавляем текст между *
                    cleaned += markdown_text.substr(i, end_italic - i);
                    i = end_italic;
                    continue;
                }
            }
        }

        // Пропускаем ~~зачеркнутый~~ текст
        if (c == '~' && i + 1 < markdown_text.length() && markdown_text[i + 1] == '~') {
            i += 2;
            size_t end_strike = markdown_text.find("~~", i);
            if (end_strike != std::string::npos) {
                cleaned += markdown_text.substr(i, end_strike - i);
                i = end_strike + 1;
                continue;
            }
        }

        // Пропускаем изображения ![alt](url)
        if (c == '!' && i + 1 < markdown_text.length() && markdown_text[i + 1] == '[') {
            size_t end_bracket = markdown_text.find(']', i + 2);
            if (end_bracket != std::string::npos && end_bracket + 1 < markdown_text.length() &&
                markdown_text[end_bracket + 1] == '(') {
                size_t url_end = markdown_text.find(')', end_bracket + 2);
                if (url_end != std::string::npos) {
                    // Пропускаем изображение полностью
                    i = url_end;
                    continue;
                }
            }
        }

        // Добавляем обычный символ
        if (!in_html_tag) {
            cleaned += c;
        }
    }

    // Обрабатываем последнюю строку
    if (!line.empty() && !in_code_block) {
        cleaned += line;
    }

    return cleaned;
}

std::vector<std::string> DocumentParser::parse_md(const std::string& file_path) {
    std::vector<std::string> paragraphs;

    // Проверяем, что путь к файлу не пуст
    if (file_path.empty()) {
        std::cerr << "Error: Empty file path provided for MD parsing" << std::endl;
        return paragraphs;
    }

    // Проверяем существование файла
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << file_path << std::endl;
        return paragraphs;
    }

    // Проверяем, что файл не пуст
    if (file.peek() == std::ifstream::traits_type::eof()) {
        std::cerr << "Warning: File is empty: " << file_path << std::endl;
        file.close();
        return paragraphs;
    }

    // Читаем всё содержимое файла
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    std::cout << "Parsing MD file: " << file_path << " (" << content.length() << " bytes)" << std::endl;

    // Очищаем Markdown-разметку
    std::string cleaned_content = clean_markdown(content);

    // Разбиваем на параграфы по пустым строкам
    std::string current_paragraph;
    for (size_t i = 0; i < cleaned_content.length(); ++i) {
        char c = cleaned_content[i];

        if (c == '\n') {
            if (current_paragraph.empty()) {
                // Пустая строка - конец параграфа
                continue;
            } else {
                // Проверяем, будет ли следующая строка пустой
                if (i + 1 < cleaned_content.length() && cleaned_content[i + 1] == '\n') {
                    // Двойной перенос - конец параграфа
                    paragraphs.push_back(current_paragraph);
                    current_paragraph.clear();
                    i++; // Пропускаем второй \n
                } else {
                    // Одиночный перенос - продолжаем параграф
                    current_paragraph += ' ';
                }
            }
        } else {
            current_paragraph += c;
        }
    }

    // Добавляем последний параграф
    if (!current_paragraph.empty()) {
        paragraphs.push_back(current_paragraph);
    }

    // Если параграфов нет, но текст есть - добавляем весь текст как один параграф
    if (paragraphs.empty() && !cleaned_content.empty()) {
        paragraphs.push_back(cleaned_content);
    }

    std::cout << "Successfully parsed " << paragraphs.size() << " paragraph(s) from MD file" << std::endl;

    return paragraphs;
}

std::vector<std::string> DocumentParser::parse_document(const std::string& file_path) {
    // Проверяем, что путь к файлу не пуст
    if (file_path.empty()) {
        std::cerr << "Error: Empty file path provided for parsing" << std::endl;
        return std::vector<std::string>();
    }

    // Проверяем существование файла
    std::ifstream file(file_path);
    if (!file.good()) {
        std::cerr << "Error: Document file does not exist or is not accessible: " << file_path << std::endl;
        return std::vector<std::string>();
    }
    file.close();

    std::string ext = get_file_extension(file_path);

    // Проверяем, что расширение файла не пустое
    if (ext.empty()) {
        std::cerr << "Error: Could not determine file extension for: " << file_path << std::endl;
        return std::vector<std::string>();
    }

    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Check if this is a code file
    ContentType content_type = detect_content_type(file_path);
    if (content_type == ContentType::Code) {
        return parse_code(file_path);
    }

    if (ext == ".txt") {
        return parse_txt(file_path);
    } else if (ext == ".md") {
        return parse_md(file_path);
    } else if (ext == ".pdf") {
        std::cerr << "Warning: PDF parsing not implemented yet. Using placeholder for " << file_path << std::endl;
        return parse_pdf(file_path);
    } else if (ext == ".docx") {
        std::cerr << "Warning: DOCX parsing not implemented yet. Using placeholder for " << file_path << std::endl;
        return parse_docx(file_path);
    } else {
        std::cerr << "Error: Unsupported file format: " << ext << " for file: " << file_path << std::endl;
        return std::vector<std::string>();
    }
}

std::string DocumentParser::get_file_extension(const std::string& file_path) {
    size_t pos = file_path.find_last_of('.');
    if (pos != std::string::npos) {
        return file_path.substr(pos);
    }
    return "";
}

DocumentParser::ContentType DocumentParser::detect_content_type(const std::string& file_path) {
    std::string lang = get_language(file_path);
    if (!lang.empty()) {
        return ContentType::Code;
    }

    std::string ext = get_file_extension(file_path);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Text formats
    if (ext == ".txt" || ext == ".md" || ext == ".pdf" || ext == ".docx" ||
        ext == ".html" || ext == ".htm" || ext == ".csv" || ext == ".json" ||
        ext == ".xml" || ext == ".log" || ext == ".rst" || ext == ".adoc") {
        return ContentType::Text;
    }

    return ContentType::Text;  // Default to text
}

std::string DocumentParser::get_language(const std::string& file_path) {
    std::string ext = get_file_extension(file_path);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".h") return "cpp";
    if (ext == ".c") return "c";
    if (ext == ".py") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".go") return "go";
    if (ext == ".js") return "javascript";
    if (ext == ".ts") return "typescript";
    if (ext == ".java") return "java";
    if (ext == ".cs") return "csharp";
    if (ext == ".rb") return "ruby";
    if (ext == ".php") return "php";
    if (ext == ".swift") return "swift";
    if (ext == ".kt" || ext == ".kts") return "kotlin";
    if (ext == ".sh" || ext == ".bash") return "shell";
    if (ext == ".sql") return "sql";
    if (ext == ".r" || ext == ".R") return "r";

    return "";
}

std::vector<std::string> DocumentParser::parse_code(const std::string& file_path) {
    std::string language = get_language(file_path);
    if (language.empty()) {
        // Not a recognized code file, fall back to text parsing
        return parse_txt(file_path);
    }

    // Try AST parsing first if tree-sitter is available
    auto ast_result = parse_code_ast(file_path, language);
    if (!ast_result.empty()) {
        return ast_result;
    }

    // Fallback to heuristic parsing
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open code file " << file_path << std::endl;
        return {};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    return split_code_by_blocks(content, language);
}

std::vector<std::string> DocumentParser::parse_code_ast(const std::string& file_path, const std::string& language) {
#ifdef ENABLE_AST_PARSING
    if (!AstParser::is_available(language)) {
        return {};
    }

    try {
        AstParser parser;
        AstNode root = parser.parse_file(file_path, language);

        std::vector<std::string> result;

        // Extract preamble (includes/imports)
        AstNode preamble = parser.extract_preamble(root, root.content, language);
        if (!preamble.content.empty()) {
            auto preamble_chunks = parser.nodes_to_chunks({preamble}, file_path);
            result.insert(result.end(), preamble_chunks.begin(), preamble_chunks.end());
        }

        // Extract top-level nodes
        auto nodes = parser.extract_top_level_nodes(root);

        // Split large nodes
        std::vector<AstNode> final_nodes;
        int max_tokens = 256; // Default chunk size
        for (const auto& node : nodes) {
            auto chunks = parser.split_large_node(node, max_tokens);
            for (auto& chunk : chunks) {
                final_nodes.push_back(std::move(chunk));
            }
        }

        // Convert to chunks with metadata (including file_path)
        auto code_chunks = parser.nodes_to_chunks(final_nodes, file_path);
        result.insert(result.end(), code_chunks.begin(), code_chunks.end());

        return result;
    } catch (const std::exception& e) {
        std::cerr << "[AST] Parse error, falling back to heuristic: " << e.what() << std::endl;
        return {};
    }
#else
    return {};
#endif
}

std::vector<std::string> DocumentParser::split_code_by_blocks(
    const std::string& content, const std::string& language)
{
    std::vector<std::string> blocks;

    // Simple brace-based splitting for C-like languages
    // More sophisticated AST parsing would require tree-sitter
    if (language == "cpp" || language == "c" || language == "java" ||
        language == "csharp" || language == "go" || language == "rust" ||
        language == "javascript" || language == "typescript") {

        // Split by top-level braces (functions, classes, namespaces)
        int brace_depth = 0;
        int block_start = 0;
        int line_num = 1;
        bool in_block = false;
        std::string current_symbol;

        for (size_t i = 0; i < content.size(); ++i) {
            if (content[i] == '\n') line_num++;

            if (content[i] == '{') {
                if (brace_depth == 0) {
                    block_start = i > 0 ? i - 1 : 0;
                    // Try to extract symbol name from preceding line
                    size_t line_start = content.rfind('\n', block_start);
                    if (line_start == std::string::npos) line_start = 0;
                    else line_start++;
                    std::string decl = content.substr(line_start, block_start - line_start);
                    // Simple heuristic: extract identifier before '('
                    size_t paren_pos = decl.find('(');
                    if (paren_pos != std::string::npos) {
                        size_t name_end = paren_pos;
                        while (name_end > 0 && (decl[name_end-1] == ' ' || decl[name_end-1] == '*' || decl[name_end-1] == '&')) name_end--;
                        size_t name_start = name_end;
                        while (name_start > 0 && decl[name_start-1] != ' ' && decl[name_start-1] != '\t') name_start--;
                        current_symbol = decl.substr(name_start, name_end - name_start);
                    }
                    in_block = true;
                }
                brace_depth++;
            } else if (content[i] == '}') {
                brace_depth--;
                if (brace_depth == 0 && in_block) {
                    std::string block = content.substr(block_start, i - block_start + 1);
                    // Prefix with metadata
                    std::string prefix = "[[" + std::to_string(line_num) + ":" + current_symbol + "]]";
                    blocks.push_back(prefix + block);
                    in_block = false;
                    current_symbol.clear();
                }
            }
        }

        // If there's remaining content before the first block (includes, etc.)
        if (!blocks.empty() && block_start > 0) {
            std::string preamble = content.substr(0, block_start);
            if (!preamble.empty()) {
                blocks.insert(blocks.begin(), "[[preamble]]" + preamble);
            }
        }
    } else {
        // For Python and other languages, split by top-level definitions
        std::istringstream iss(content);
        std::string line;
        std::string current_block;
        int line_num = 0;
        int indent_level = 0;

        while (std::getline(iss, line)) {
            line_num++;

            // Detect top-level definitions (class, def, function)
            bool is_def = false;
            std::string symbol;

            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));

            if (language == "python") {
                if (trimmed.substr(0, 6) == "class " || trimmed.substr(0, 4) == "def ") {
                    is_def = true;
                    size_t name_start = trimmed.find(' ') + 1;
                    size_t name_end = trimmed.find('(', name_start);
                    if (name_end == std::string::npos) name_end = trimmed.find(':', name_start);
                    if (name_end != std::string::npos) {
                        symbol = trimmed.substr(name_start, name_end - name_start);
                    }
                }
            }

            if (is_def && !current_block.empty()) {
                // Save previous block
                std::string prefix = "[[" + std::to_string(line_num) + ":" + symbol + "]]";
                blocks.push_back(prefix + current_block);
                current_block.clear();
            }

            current_block += line + "\n";
        }

        // Add last block
        if (!current_block.empty()) {
            blocks.push_back("[[end]]" + current_block);
        }
    }

    // If no blocks were found, return the whole file as one block
    if (blocks.empty() && !content.empty()) {
        blocks.push_back("[[full]]" + content);
    }

    return blocks;
}

} // namespace core
} // namespace llama_gui