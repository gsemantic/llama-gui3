#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

class DocumentParser {
public:
    static std::vector<std::string> parse_txt(const std::string& file_path);
    static std::vector<std::string> parse_pdf(const std::string& file_path);
    static std::vector<std::string> parse_docx(const std::string& file_path);
    static std::vector<std::string> parse_md(const std::string& file_path);

    // Универсальный метод для определения типа файла и его парсинга
    static std::vector<std::string> parse_document(const std::string& file_path);

    // Code-aware methods
    enum class ContentType { Text, Code, Mixed };

    static ContentType detect_content_type(const std::string& file_path);
    static std::string get_language(const std::string& file_path);

    // Parse code file into logical blocks (functions, classes, etc.)
    // Returns blocks with metadata encoded as prefix: "[[start_line:end_line:symbol_name]]content"
    // Uses tree-sitter AST when available, falls back to heuristic parsing
    static std::vector<std::string> parse_code(const std::string& file_path);

private:
    static std::string get_file_extension(const std::string& file_path);
    static std::vector<std::string> split_code_by_blocks(const std::string& content, const std::string& language);
    static std::vector<std::string> parse_code_ast(const std::string& file_path, const std::string& language);
};

} // namespace core
} // namespace llama_gui
