#include "ast_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <cstring>
#include <climits>

// Include tree-sitter API only in implementation
#include <tree_sitter/api.h>

// Tree-sitter language extern declarations
extern "C" {
    const TSLanguage *tree_sitter_c(void);
    const TSLanguage *tree_sitter_cpp(void);
    const TSLanguage *tree_sitter_python(void);
    const TSLanguage *tree_sitter_rust(void);
}

namespace llama_gui {
namespace core {

// Language to tree-sitter grammar mapping
static const std::unordered_map<std::string, const TSLanguage* (*)()> language_grammars = {
    {"c", tree_sitter_c},
    {"cpp", tree_sitter_cpp},
    {"python", tree_sitter_python},
    {"rust", tree_sitter_rust},
};

// Node type names for code elements (only top-level definitions with bodies)
static const std::vector<std::string> function_types = {
    "function_definition",   // C/C++/Python: has body
    "function_item",         // Rust: has body
    "method_definition",     // Python: has body
    "method_declaration"     // C++: can be declaration only
};

static const std::vector<std::string> class_types = {
    "class_definition", "class_specifier", "class_declaration",
    "struct_specifier", "struct_declaration",
    "interface_declaration", "impl_item", "trait_item"
};

static const std::vector<std::string> namespace_types = {
    "namespace_definition", "namespace_declaration",
    "mod_item"
};

static const std::vector<std::string> enum_types = {
    "enum_declaration", "enum_definition", "enum_specifier"
};

static const std::vector<std::string> typedef_types = {
    "type_definition", "type_alias", "typedef_declaration",
    "alias_declaration"
};

static const std::vector<std::string> macro_types = {
    "macro_definition", "macro_declaration"
};

// Node types to SKIP (noise, not useful as separate chunks)
static const std::vector<std::string> skip_types = {
    "storage_class_specifier",   // static, extern, inline, etc.
    "function_declarator",       // function signature without body
    "parameter_list",            // (param1, param2)
    "parameter_declaration",     // int x
    "compound_statement",        // { ... }
    "expression_statement",      // single expression
    "return_statement",          // return expr
    "break_statement",           // break
    "continue_statement",        // continue
    "comment",                   // comments
    "type_identifier",           // type names
    "field_identifier",          // field names
    "number_literal",            // numbers
    "string_literal",            // strings
    "true", "false", "null", "nullptr", "this", "self"
};

static bool is_skip_type(const std::string& node_type) {
    for (const auto& t : skip_types) {
        if (node_type == t) return true;
    }
    return false;
}

static bool is_code_node(const std::string& node_type) {
    // Skip noise types
    if (is_skip_type(node_type)) return false;

    for (const auto& t : function_types) {
        if (node_type.find(t) != std::string::npos) return true;
    }
    for (const auto& t : class_types) {
        if (node_type.find(t) != std::string::npos) return true;
    }
    for (const auto& t : namespace_types) {
        if (node_type.find(t) != std::string::npos) return true;
    }
    for (const auto& t : enum_types) {
        if (node_type.find(t) != std::string::npos) return true;
    }
    for (const auto& t : typedef_types) {
        if (node_type.find(t) != std::string::npos) return true;
    }
    for (const auto& t : macro_types) {
        if (node_type.find(t) != std::string::npos) return true;
    }
    // Python decorated definitions
    if (node_type == "decorated_definition") return true;
    return false;
}

struct AstParser::Impl {
    TSParser* parser = nullptr;

    Impl() {
        parser = ts_parser_new();
    }

    ~Impl() {
        if (parser) {
            ts_parser_delete(parser);
        }
    }
};

AstParser::AstParser() : impl_(std::make_unique<Impl>()) {}
AstParser::~AstParser() = default;

bool AstParser::is_available(const std::string& language) {
    return language_grammars.find(language) != language_grammars.end();
}

std::vector<std::string> AstParser::supported_languages() {
    std::vector<std::string> langs;
    for (const auto& [lang, _] : language_grammars) {
        langs.push_back(lang);
    }
    return langs;
}

AstNode AstParser::parse_file(const std::string& file_path, const std::string& language) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "[AST] Could not open file: " << file_path << std::endl;
        return {};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_source(buffer.str(), language);
}

// Helper functions for AST extraction
static std::string get_node_source(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start < source.length() && end <= source.length()) {
        return source.substr(start, end - start);
    }
    return "";
}

static std::string get_node_name(TSNode node, const std::string& source) {
    // Try to find a "name" child node
    TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name_node)) {
        return get_node_source(name_node, source);
    }

    const char* node_type = ts_node_type(node);
    std::string node_type_str(node_type);

    // For function_definition, look inside function_declarator for identifier
    if (node_type_str.find("function_definition") != std::string::npos) {
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_child(node, i);
            const char* child_type = ts_node_type(child);
            std::string child_type_str(child_type);
            if (child_type_str == "function_declarator") {
                uint32_t sub_count = ts_node_child_count(child);
                for (uint32_t j = 0; j < sub_count; ++j) {
                    TSNode sub = ts_node_child(child, j);
                    const char* sub_type = ts_node_type(sub);
                    std::string sub_type_str(sub_type);
                    if (sub_type_str == "identifier" || sub_type_str == "field_identifier") {
                        return get_node_source(sub, source);
                    }
                }
            }
        }
    }

    // For function/method declarators, try to find the identifier
    if (node_type_str.find("function") != std::string::npos ||
        node_type_str.find("method") != std::string::npos ||
        node_type_str.find("impl_item") != std::string::npos) {
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_child(node, i);
            const char* child_type = ts_node_type(child);
            std::string child_type_str(child_type);
            if (child_type_str == "identifier" || child_type_str == "field_identifier" ||
                child_type_str == "type_identifier") {
                return get_node_source(child, source);
            }
        }
    }

    // For enum/typedef, try to find type name
    for (const auto& et : enum_types) {
        if (node_type_str.find(et) != std::string::npos) {
            TSNode name = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name)) {
                return get_node_source(name, source);
            }
            // Try type_identifier child
            uint32_t child_count = ts_node_child_count(node);
            for (uint32_t i = 0; i < child_count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* child_type = ts_node_type(child);
                if (strcmp(child_type, "type_identifier") == 0) {
                    return get_node_source(child, source);
                }
            }
        }
    }

    return "";
}

static std::string get_parent_name(TSNode node, const std::string& source) {
    TSNode current = ts_node_parent(node);
    while (!ts_node_is_null(current)) {
        const char* parent_type = ts_node_type(current);
        std::string parent_type_str(parent_type);
        for (const auto& ct : class_types) {
            if (parent_type_str.find(ct) != std::string::npos) {
                return get_node_name(current, source);
            }
        }
        for (const auto& nt : namespace_types) {
            if (parent_type_str.find(nt) != std::string::npos) {
                std::string ns_name = get_node_name(current, source);
                if (!ns_name.empty()) return ns_name;
            }
        }
        current = ts_node_parent(current);
    }
    return "";
}

static void extract_nodes_recursive(TSNode node, AstNode& parent, const std::string& source, int depth = 0) {
    if (depth > 50) return;

    uint32_t child_count = ts_node_child_count(node);
    const char* node_type_cstr = ts_node_type(node);
    std::string node_type(node_type_cstr);

    if (is_code_node(node_type)) {
        AstNode ast_node;
        ast_node.type = node_type;
        ast_node.name = get_node_name(node, source);
        ast_node.parent_name = get_parent_name(node, source);
        ast_node.start_line = static_cast<int>(ts_node_start_point(node).row) + 1;
        ast_node.end_line = static_cast<int>(ts_node_end_point(node).row) + 1;
        ast_node.start_column = static_cast<int>(ts_node_start_point(node).column);
        ast_node.end_column = static_cast<int>(ts_node_end_point(node).column);
        ast_node.content = get_node_source(node, source);

        // For decorated_definition, look for the actual definition inside
        if (node_type == "decorated_definition") {
            for (uint32_t i = 0; i < child_count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* child_type = ts_node_type(child);
                std::string child_type_str(child_type);
                if (child_type_str.find("definition") != std::string::npos ||
                    child_type_str.find("declaration") != std::string::npos) {
                    ast_node.name = get_node_name(child, source);
                    break;
                }
            }
        }

        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_child(node, i);
            extract_nodes_recursive(child, ast_node, source, depth + 1);
        }

        parent.children.push_back(std::move(ast_node));
    } else {
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_child(node, i);
            extract_nodes_recursive(child, parent, source, depth + 1);
        }
    }
}

AstNode AstParser::parse_source(const std::string& source, const std::string& language) {
    AstNode root;
    root.type = "program";
    root.content = source;

    auto it = language_grammars.find(language);
    if (it == language_grammars.end()) {
        std::cerr << "[AST] No tree-sitter grammar for language: " << language << std::endl;
        return root;
    }

    ts_parser_set_language(impl_->parser, it->second());

    TSTree* tree = ts_parser_parse_string(
        impl_->parser,
        nullptr,
        source.c_str(),
        static_cast<uint32_t>(source.length())
    );

    if (!tree) {
        std::cerr << "[AST] Failed to parse source code" << std::endl;
        return root;
    }

    TSNode ts_root = ts_tree_root_node(tree);
    extract_nodes_recursive(ts_root, root, source);
    ts_tree_delete(tree);

    return root;
}

std::vector<AstNode> AstParser::extract_top_level_nodes(const AstNode& root) const {
    return root.children;
}

AstNode AstParser::extract_preamble(const AstNode& root, const std::string& source, const std::string& language) const {
    AstNode preamble;
    preamble.type = "preamble";
    preamble.name = language + "_imports";

    // Find the line of the first code node
    int first_code_line = INT_MAX;
    for (const auto& child : root.children) {
        if (child.start_line > 0 && child.start_line < first_code_line) {
            first_code_line = child.start_line;
        }
    }

    if (first_code_line == INT_MAX || first_code_line <= 1) {
        return preamble; // Empty preamble
    }

    // Extract everything before the first code node
    std::istringstream iss(source);
    std::string line;
    int line_num = 1;
    std::string preamble_content;

    while (std::getline(iss, line) && line_num < first_code_line) {
        // Skip empty lines at the start
        if (!preamble_content.empty() || !line.empty()) {
            preamble_content += line + "\n";
        }
        line_num++;
    }

    // Trim trailing whitespace
    while (!preamble_content.empty() && (preamble_content.back() == '\n' || preamble_content.back() == '\r')) {
        preamble_content.pop_back();
    }

    preamble.content = preamble_content;
    preamble.start_line = 1;
    preamble.end_line = first_code_line - 1;

    return preamble;
}

// Helper: find balanced block boundaries in source code
static std::pair<int, int> find_block_end(const std::string& content, int start_pos) {
    int brace_count = 0;
    bool in_string = false;
    char string_char = 0;
    bool in_comment = false;
    bool in_line_comment = false;

    for (size_t i = start_pos; i < content.size(); ++i) {
        char c = content[i];
        char prev = (i > 0) ? content[i-1] : 0;

        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_comment) {
            if (c == '/' && prev == '*') in_comment = false;
            continue;
        }
        if (in_string) {
            if (c == string_char && prev != '\\') in_string = false;
            continue;
        }

        if (c == '/' && i + 1 < content.size()) {
            if (content[i+1] == '/') { in_line_comment = true; continue; }
            if (content[i+1] == '*') { in_comment = true; i++; continue; }
        }
        if (c == '"' || c == '\'' || c == '`') {
            in_string = true;
            string_char = c;
            continue;
        }

        if (c == '{') brace_count++;
        else if (c == '}') {
            brace_count--;
            if (brace_count == 0) {
                return {static_cast<int>(i), 0};
            }
        }
    }
    return {-1, 0};
}

// Helper: split content by logical code blocks (functions, if/else, loops)
static std::vector<std::pair<int, int>> find_code_blocks(const std::string& content, int start_line) {
    std::vector<std::pair<int, int>> blocks;
    std::istringstream iss(content);
    std::string line;
    int line_num = start_line;
    int block_start = -1;
    int brace_depth = 0;

    while (std::getline(iss, line)) {
        // Track brace depth
        for (char c : line) {
            if (c == '{') brace_depth++;
            else if (c == '}') {
                brace_depth--;
                if (brace_depth == 0 && block_start >= 0) {
                    // End of a top-level block
                    blocks.push_back({block_start, line_num});
                    block_start = -1;
                }
            }
        }

        // Detect start of a new block (function, if, for, while, etc.)
        if (brace_depth == 0) {
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            
            bool is_block_start = false;
            // Check for common block starters
            if (trimmed.find("if ") == 0 || trimmed.find("if(") == 0 ||
                trimmed.find("else") == 0 ||
                trimmed.find("for ") == 0 || trimmed.find("for(") == 0 ||
                trimmed.find("while ") == 0 || trimmed.find("while(") == 0 ||
                trimmed.find("switch ") == 0 || trimmed.find("switch(") == 0 ||
                trimmed.find("try") == 0 || trimmed.find("catch") == 0 ||
                trimmed.find("do ") == 0 || trimmed.find("do{") == 0 ||
                (trimmed.find('{') != std::string::npos && brace_depth == 1)) {
                is_block_start = true;
            }

            // Also check for function-like patterns (type name(...) {)
            if (!is_block_start && trimmed.find('{') != std::string::npos) {
                size_t paren = trimmed.find('(');
                if (paren != std::string::npos && paren < trimmed.find('{')) {
                    is_block_start = true;
                }
            }

            if (is_block_start && block_start < 0) {
                block_start = line_num;
            }
        }

        line_num++;
    }

    // Handle unterminated block
    if (block_start >= 0) {
        blocks.push_back({block_start, line_num - 1});
    }

    return blocks;
}

std::vector<AstNode> AstParser::split_large_node(const AstNode& node, int max_tokens, int max_depth) const {
    std::vector<AstNode> result;

    int node_tokens = node.estimate_tokens();
    if (node_tokens <= max_tokens || max_depth <= 0) {
        result.push_back(node);
        return result;
    }

    if (!node.children.empty()) {
        // First try to recursively split each child
        bool any_child_split = false;
        for (const auto& child : node.children) {
            auto child_chunks = split_large_node(child, max_tokens, max_depth - 1);
            if (child_chunks.size() > 1) any_child_split = true;
            for (auto& chunk : child_chunks) {
                if (chunk.parent_name.empty()) {
                    chunk.parent_name = node.name;
                }
                result.push_back(std::move(chunk));
            }
        }

        // If any child was split, we're done
        if (any_child_split) {
            return result;
        }

        // If all children fit in one chunk, group them
        if (result.size() == 1 && result[0].estimate_tokens() <= max_tokens) {
            return result;
        }

        // Otherwise, group children into chunks by token budget
        result.clear();
        AstNode current_chunk;
        current_chunk.type = node.type + "_part";
        current_chunk.parent_name = node.name;
        current_chunk.start_line = node.start_line;

        for (const auto& child : node.children) {
            int child_tokens = child.estimate_tokens();
            int current_tokens = current_chunk.estimate_tokens();

            if (current_tokens + child_tokens > max_tokens && !current_chunk.content.empty()) {
                current_chunk.end_line = child.start_line - 1;
                result.push_back(std::move(current_chunk));
                current_chunk = AstNode();
                current_chunk.type = node.type + "_part";
                current_chunk.parent_name = node.name;
                current_chunk.start_line = child.start_line;
            }

            current_chunk.children.push_back(child);
            current_chunk.content += child.content + "\n";
            current_chunk.name = node.name;
        }

        if (!current_chunk.content.empty()) {
            current_chunk.end_line = node.end_line;
            result.push_back(std::move(current_chunk));
        }

        // Recursively split any resulting chunks that are still too large
        std::vector<AstNode> final_result;
        for (auto& chunk : result) {
            if (chunk.estimate_tokens() > max_tokens) {
                auto sub_chunks = split_large_node(chunk, max_tokens, max_depth - 1);
                final_result.insert(final_result.end(), sub_chunks.begin(), sub_chunks.end());
            } else {
                final_result.push_back(std::move(chunk));
            }
        }
        return final_result;

    } else {
        // No children - try to split by logical code blocks first
        auto blocks = find_code_blocks(node.content, node.start_line);
        
        if (blocks.size() > 1) {
            // Split by detected blocks
            std::istringstream iss(node.content);
            std::string line;
            std::string all_content = node.content;
            int line_num = node.start_line;

            for (const auto& [block_start, block_end] : blocks) {
                std::string block_content;
                int current_line = node.start_line;
                std::istringstream line_iss(all_content);

                while (std::getline(line_iss, line)) {
                    if (current_line >= block_start && current_line <= block_end) {
                        block_content += line + "\n";
                    }
                    current_line++;
                    if (current_line > block_end) break;
                }

                if (!block_content.empty()) {
                    // Trim trailing newlines
                    while (!block_content.empty() && block_content.back() == '\n') {
                        block_content.pop_back();
                    }

                    AstNode chunk;
                    chunk.type = node.type + "_block";
                    chunk.name = node.name;
                    chunk.parent_name = node.parent_name;
                    chunk.start_line = block_start;
                    chunk.end_line = block_end;
                    chunk.content = block_content;

                    // Recursively split if still too large
                    if (chunk.estimate_tokens() > max_tokens) {
                        auto sub_chunks = split_large_node(chunk, max_tokens, max_depth - 1);
                        result.insert(result.end(), sub_chunks.begin(), sub_chunks.end());
                    } else {
                        result.push_back(std::move(chunk));
                    }
                }
            }

            if (!result.empty()) {
                return result;
            }
        }

        // Fallback: split by lines. Одна гигантская строка без переводов не
        // делится по getline — её дополнительно режем по символьному бюджету.
        const int max_chunk_chars = std::max(1, max_tokens * 4); // ~4 симв/токен (ASCII)

        std::istringstream iss(node.content);
        std::string line;
        std::string current_chunk;
        int line_count = 0;
        int chunk_start_line = node.start_line;

        auto emit_chunk = [&](int end_line) {
            while (!current_chunk.empty() && current_chunk.back() == '\n') {
                current_chunk.pop_back();
            }
            if (current_chunk.empty()) return;
            AstNode chunk;
            chunk.type = node.type + "_part";
            chunk.name = node.name;
            chunk.parent_name = node.parent_name;
            chunk.start_line = chunk_start_line;
            chunk.end_line = end_line;
            chunk.content = current_chunk;
            result.push_back(std::move(chunk));
            current_chunk.clear();
        };

        while (std::getline(iss, line)) {
            const std::string line_with_nl = line + "\n";

            // Длинная строка без переносов: режем по символам.
            if (static_cast<int>(line_with_nl.size()) >= max_chunk_chars) {
                emit_chunk(chunk_start_line + line_count - 1);
                chunk_start_line += line_count;
                line_count = 0;

                size_t pos = 0;
                while (pos < line_with_nl.size()) {
                    const size_t len = std::min(static_cast<size_t>(max_chunk_chars),
                                                line_with_nl.size() - pos);
                    AstNode piece;
                    piece.type = node.type + "_part";
                    piece.name = node.name;
                    piece.parent_name = node.parent_name;
                    piece.start_line = chunk_start_line;
                    piece.end_line = chunk_start_line;
                    piece.content = line_with_nl.substr(pos, len);
                    result.push_back(std::move(piece));
                    pos += len;
                }
                chunk_start_line++;
                continue;
            }

            // Накапливаем строки, не выходя за токен-бюджет чанка.
            if (!current_chunk.empty() &&
                static_cast<int>(current_chunk.size() + line_with_nl.size()) > max_chunk_chars) {
                emit_chunk(chunk_start_line + line_count - 1);
                chunk_start_line += line_count;
                line_count = 0;
            }

            current_chunk += line_with_nl;
            line_count++;

            // Split at ~50 lines or when we hit a blank line after code
            bool split_here = (line_count >= 50) ||
                              (line_count > 10 && line.empty() && !current_chunk.empty());

            if (split_here) {
                emit_chunk(chunk_start_line + line_count - 1);
                chunk_start_line += line_count;
                line_count = 0;
            }
        }

        if (!current_chunk.empty()) {
            emit_chunk(node.end_line);
        }

        // Recursively split any chunks that are still too large
        std::vector<AstNode> final_result;
        for (auto& chunk : result) {
            if (chunk.estimate_tokens() > max_tokens) {
                auto sub_chunks = split_large_node(chunk, max_tokens, max_depth - 1);
                final_result.insert(final_result.end(), sub_chunks.begin(), sub_chunks.end());
            } else {
                final_result.push_back(std::move(chunk));
            }
        }
        return final_result;
    }

    return result;
}

std::vector<std::string> AstParser::nodes_to_chunks(const std::vector<AstNode>& nodes, const std::string& file_path) const {
    std::vector<std::string> chunks;

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        // Format: [[start:end:name:parent:file_path]]content
        std::string prefix = "[[" + std::to_string(node.start_line) + ":" +
                            std::to_string(node.end_line) + ":" +
                            node.name + ":" + node.parent_name;
        if (!file_path.empty()) {
            prefix += ":" + file_path;
        }
        prefix += "]]";
        chunks.push_back(prefix + node.content);
    }

    return chunks;
}

int AstNode::estimate_tokens() const {
    if (content.empty()) return 0;
    int ascii = 0, non_ascii = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(content[i]);
        if (c < 128) ascii++;
        else if (c >= 0xC0) non_ascii++;
    }
    return std::max(1, (ascii / 4) + (non_ascii / 2));
}

} // namespace core
} // namespace llama_gui
