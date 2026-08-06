#pragma once

#include <string>
#include <vector>
#include <memory>

namespace llama_gui {
namespace core {

/**
 * @brief AST node representing a parsed code element
 */
struct AstNode {
    std::string type;           // "function_definition", "class_definition", "method_definition", etc.
    std::string name;           // symbol name (function, class, method name)
    std::string parent_name;    // parent context (class name for a method)
    int start_line = 0;
    int end_line = 0;
    int start_column = 0;
    int end_column = 0;
    std::string content;        // source code of this node
    std::vector<AstNode> children;

    // Helper to estimate token count
    int estimate_tokens() const;
};

/**
 * @brief AST-aware code parser using tree-sitter
 *
 * Parses source code into an AST and extracts semantic blocks
 * (functions, classes, methods) with full metadata.
 * Falls back to heuristic parsing if tree-sitter is unavailable.
 */
class AstParser {
public:
    AstParser();
    ~AstParser();

    /**
     * @brief Parse a source file into AST
     */
    AstNode parse_file(const std::string& file_path, const std::string& language);

    /**
     * @brief Parse source code string into AST
     */
    AstNode parse_source(const std::string& source, const std::string& language);

    /**
     * @brief Extract top-level semantic blocks from AST
     */
    std::vector<AstNode> extract_top_level_nodes(const AstNode& root) const;

    /**
     * @brief Extract includes/imports as a preamble chunk
     */
    AstNode extract_preamble(const AstNode& root, const std::string& source, const std::string& language) const;

    /**
     * @brief Split a large node into smaller sub-nodes
     * @param max_depth Maximum recursion depth (default 5)
     */
    std::vector<AstNode> split_large_node(const AstNode& node, int max_tokens, int max_depth = 5) const;

    /**
     * @brief Convert AST nodes to RAG-compatible chunk strings
     * @param nodes AST nodes to convert
     * @param file_path Optional file path to include in metadata
     */
    std::vector<std::string> nodes_to_chunks(const std::vector<AstNode>& nodes, const std::string& file_path = "") const;

    /**
     * @brief Check if tree-sitter is available for a language
     */
    static bool is_available(const std::string& language);

    /**
     * @brief Get list of supported languages
     */
    static std::vector<std::string> supported_languages();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
} // namespace llama_gui
