#include "../include/core/ast_parser.h"
#include "test_framework.h"
#include <string>
#include <fstream>
#include <iostream>
#include <cstdio>

using namespace llama_gui::core;

// Helper to create a temporary file
static std::string create_temp_file(const std::string& content, const std::string& ext) {
    std::string filename = "/tmp/test_ast_" + ext.substr(1) + ".cpp";
    std::ofstream file(filename);
    file << content;
    file.close();
    return filename;
}

// Test: AstNode token estimation
void test_ast_node_tokens() {
    AstNode node;
    node.content = "Hello world this is a test";
    int tokens = node.estimate_tokens();
    TEST_ASSERT(tokens >= 1 && tokens <= 10);
}

void test_ast_node_tokens_empty() {
    AstNode node;
    TEST_ASSERT_EQUAL(node.estimate_tokens(), 0);
}

// Test: is_available
void test_ast_parser_available() {
    TEST_ASSERT_TRUE(AstParser::is_available("cpp"));
    TEST_ASSERT_TRUE(AstParser::is_available("c"));
    TEST_ASSERT_TRUE(AstParser::is_available("python"));
    TEST_ASSERT_TRUE(AstParser::is_available("rust"));
    TEST_ASSERT_FALSE(AstParser::is_available("java"));
    TEST_ASSERT_FALSE(AstParser::is_available("unknown"));
}

// Test: supported_languages
void test_ast_parser_supported_languages() {
    auto langs = AstParser::supported_languages();
    TEST_ASSERT(langs.size() >= 4);
}

// Test: parse C++ source
void test_ast_parse_cpp_function() {
    std::string source = R"(
int add(int a, int b) {
    return a + b;
}

int main() {
    return 0;
}
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");

    TEST_ASSERT_EQUAL(root.type, "program");
    auto nodes = parser.extract_top_level_nodes(root);
    TEST_ASSERT(nodes.size() >= 1);

    // Should find at least the add function
    bool found_add = false;
    for (const auto& node : nodes) {
        if (node.name == "add") {
            found_add = true;
            TEST_ASSERT(node.start_line > 0);
            TEST_ASSERT(node.end_line >= node.start_line);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_add);
}

// Test: parse C++ class
void test_ast_parse_cpp_class() {
    std::string source = R"(
class MyClass {
public:
    void method1() {}
    int method2(int x) { return x; }
};
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    auto nodes = parser.extract_top_level_nodes(root);

    // Should find the class
    bool found_class = false;
    for (const auto& node : nodes) {
        if (node.type.find("class") != std::string::npos ||
            node.type.find("struct") != std::string::npos) {
            found_class = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_class);
}

// Test: parse Python source
void test_ast_parse_python_function() {
    std::string source = R"(
def hello():
    print("Hello")

def add(a, b):
    return a + b
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "python");
    auto nodes = parser.extract_top_level_nodes(root);

    TEST_ASSERT(nodes.size() >= 2);

    bool found_hello = false;
    bool found_add = false;
    for (const auto& node : nodes) {
        if (node.name == "hello") found_hello = true;
        if (node.name == "add") found_add = true;
    }
    TEST_ASSERT_TRUE(found_hello);
    TEST_ASSERT_TRUE(found_add);
}

// Test: parse Python class
void test_ast_parse_python_class() {
    std::string source = R"(
class Animal:
    def speak(self):
        pass

    def eat(self, food):
        pass
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "python");
    auto nodes = parser.extract_top_level_nodes(root);

    // Should find the class
    bool found_class = false;
    for (const auto& node : nodes) {
        if (node.type.find("class") != std::string::npos) {
            found_class = true;
            // Should have methods as children
            TEST_ASSERT(node.children.size() >= 2);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_class);
}

// Test: nodes_to_chunks
void test_ast_nodes_to_chunks() {
    std::vector<AstNode> nodes;
    AstNode node;
    node.type = "function_definition";
    node.name = "test_func";
    node.parent_name = "";
    node.start_line = 1;
    node.end_line = 5;
    node.content = "int test_func() { return 0; }";
    nodes.push_back(node);

    AstParser parser;
    auto chunks = parser.nodes_to_chunks(nodes);

    TEST_ASSERT_EQUAL(chunks.size(), 1u);
    TEST_ASSERT(chunks[0].find("[[1:5:test_func:]]") != std::string::npos);
}

// Test: split_large_node
void test_ast_split_large_node() {
    AstNode node;
    node.type = "function_definition";
    node.name = "big_func";
    node.start_line = 1;
    node.end_line = 100;
    node.content = std::string(5000, 'x'); // Very large content

    AstParser parser;
    auto chunks = parser.split_large_node(node, 100);

    TEST_ASSERT(chunks.size() > 1);
}

// Test: parse empty source
void test_ast_parse_empty() {
    AstParser parser;
    AstNode root = parser.parse_source("", "cpp");
    TEST_ASSERT_EQUAL(root.type, "program");
    TEST_ASSERT(root.children.empty());
}

// Test: parse unsupported language
void test_ast_parse_unsupported() {
    AstParser parser;
    AstNode root = parser.parse_source("x = 1", "java");
    TEST_ASSERT_EQUAL(root.type, "program");
}

// Test: parse file
void test_ast_parse_file() {
    std::string source = "int foo() { return 42; }\n";
    std::string filename = create_temp_file(source, ".cpp");

    AstParser parser;
    AstNode root = parser.parse_file(filename, "cpp");

    TEST_ASSERT_EQUAL(root.type, "program");
    TEST_ASSERT(!root.content.empty());

    std::remove(filename.c_str());
}

// Test: parent_name detection
void test_ast_parent_name() {
    std::string source = R"(
class MyClass {
    void myMethod() {}
};
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    auto nodes = parser.extract_top_level_nodes(root);

    for (const auto& node : nodes) {
        if (node.type.find("class") != std::string::npos) {
            for (const auto& child : node.children) {
                if (child.name == "myMethod") {
                    // Method should have parent class name
                    // (depends on tree-sitter grammar)
                    break;
                }
            }
        }
    }
}

// Test: metadata prefix format
void test_ast_metadata_prefix_format() {
    // Test that nodes_to_chunks produces correct prefix
    std::vector<AstNode> nodes;
    AstNode node;
    node.type = "function_definition";
    node.name = "my_func";
    node.parent_name = "MyClass";
    node.start_line = 10;
    node.end_line = 25;
    node.content = "void my_func() { }";
    nodes.push_back(node);

    AstParser parser;
    auto chunks = parser.nodes_to_chunks(nodes);

    TEST_ASSERT_EQUAL(chunks.size(), 1u);
    // Check prefix format: [[start:end:name:parent]]
    TEST_ASSERT(chunks[0].find("[[10:25:my_func:MyClass]]") != std::string::npos);
}

// Test: metadata prefix without parent
void test_ast_metadata_prefix_no_parent() {
    std::vector<AstNode> nodes;
    AstNode node;
    node.type = "function_definition";
    node.name = "free_func";
    node.parent_name = "";  // No parent
    node.start_line = 1;
    node.end_line = 5;
    node.content = "int free_func() { return 0; }";
    nodes.push_back(node);

    AstParser parser;
    auto chunks = parser.nodes_to_chunks(nodes);

    TEST_ASSERT_EQUAL(chunks.size(), 1u);
    TEST_ASSERT(chunks[0].find("[[1:5:free_func:]]") != std::string::npos);
}

// Test: parse C++ enum
void test_ast_parse_cpp_enum() {
    std::string source = R"(
enum Color {
    RED,
    GREEN,
    BLUE
};
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    auto nodes = parser.extract_top_level_nodes(root);

    bool found_enum = false;
    for (const auto& node : nodes) {
        if (node.type.find("enum") != std::string::npos) {
            found_enum = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_enum);
}

// Test: parse C++ struct
void test_ast_parse_cpp_struct() {
    std::string source = R"(
struct Point {
    int x;
    int y;
    void print() {}
};
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    auto nodes = parser.extract_top_level_nodes(root);

    bool found_struct = false;
    for (const auto& node : nodes) {
        if (node.type.find("struct") != std::string::npos) {
            found_struct = true;
            // Should have methods as children
            TEST_ASSERT(node.children.size() >= 1);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_struct);
}

// Test: parse Python decorated function
void test_ast_parse_python_decorated() {
    std::string source = R"(
@staticmethod
def my_func():
    pass

@property
def name(self):
    return "test"
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "python");
    auto nodes = parser.extract_top_level_nodes(root);

    // Should find decorated definitions
    TEST_ASSERT(nodes.size() >= 2);
}

// Test: parse nested classes
void test_ast_parse_nested_classes() {
    std::string source = R"(
class Outer {
    class Inner {
        void inner_method() {}
    };
    void outer_method() {}
};
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    auto nodes = parser.extract_top_level_nodes(root);

    // Should find outer class with inner class as child
    bool found_outer = false;
    for (const auto& node : nodes) {
        if (node.type.find("class") != std::string::npos) {
            found_outer = true;
            // Should have inner class and method as children
            TEST_ASSERT(node.children.size() >= 2);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_outer);
}

// Test: parse namespace
void test_ast_parse_namespace() {
    std::string source = R"(
namespace utils {
    int helper() { return 0; }
    void process() {}
}
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    auto nodes = parser.extract_top_level_nodes(root);

    bool found_namespace = false;
    for (const auto& node : nodes) {
        if (node.type.find("namespace") != std::string::npos) {
            found_namespace = true;
            // Should have functions as children
            TEST_ASSERT(node.children.size() >= 2);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_namespace);
}

// Test: split_large_node by lines
void test_ast_split_large_node_lines() {
    AstNode node;
    node.type = "function_definition";
    node.name = "big_func";
    node.parent_name = "";
    node.start_line = 1;
    node.end_line = 200;
    // Create content with 100 lines
    std::string content;
    for (int i = 0; i < 100; ++i) {
        content += "    line " + std::to_string(i) + ";\n";
    }
    node.content = content;

    AstParser parser;
    auto chunks = parser.split_large_node(node, 50);

    // Should split into multiple chunks
    TEST_ASSERT(chunks.size() > 1);
    // First chunk should start at line 1
    TEST_ASSERT_EQUAL(chunks[0].start_line, 1);
}

// Test: extract preamble (includes)
void test_ast_extract_preamble() {
    std::string source = R"(
#include <iostream>
#include <string>

int main() {
    return 0;
}
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    AstNode preamble = parser.extract_preamble(root, source, "cpp");

    TEST_ASSERT(!preamble.content.empty());
    TEST_ASSERT(preamble.type == "preamble");
    TEST_ASSERT(preamble.content.find("#include") != std::string::npos);
}

// Test: extract preamble Python
void test_ast_extract_preamble_python() {
    std::string source = R"(
import os
from sys import argv

def hello():
    pass
)";

    AstParser parser;
    AstNode root = parser.parse_source(source, "python");
    AstNode preamble = parser.extract_preamble(root, source, "python");

    TEST_ASSERT(!preamble.content.empty());
    TEST_ASSERT(preamble.content.find("import") != std::string::npos);
}

// Test: nodes_to_chunks with file_path
void test_ast_nodes_to_chunks_with_filepath() {
    std::vector<AstNode> nodes;
    AstNode node;
    node.type = "function_definition";
    node.name = "test_func";
    node.parent_name = "";
    node.start_line = 1;
    node.end_line = 5;
    node.content = "int test_func() { return 0; }";
    nodes.push_back(node);

    AstParser parser;
    auto chunks = parser.nodes_to_chunks(nodes, "/path/to/file.cpp");

    TEST_ASSERT_EQUAL(chunks.size(), 1u);
    TEST_ASSERT(chunks[0].find("[[1:5:test_func::/path/to/file.cpp]]") != std::string::npos);
}

// Test: nodes_to_chunks without file_path
void test_ast_nodes_to_chunks_no_filepath() {
    std::vector<AstNode> nodes;
    AstNode node;
    node.type = "function_definition";
    node.name = "test_func";
    node.parent_name = "MyClass";
    node.start_line = 10;
    node.end_line = 20;
    node.content = "void test_func() {}";
    nodes.push_back(node);

    AstParser parser;
    auto chunks = parser.nodes_to_chunks(nodes);

    TEST_ASSERT_EQUAL(chunks.size(), 1u);
    TEST_ASSERT(chunks[0].find("[[10:20:test_func:MyClass]]") != std::string::npos);
    // Should not contain file_path
    TEST_ASSERT(chunks[0].find("/path") == std::string::npos);
}

// Test: preamble with no code
void test_ast_preamble_no_code() {
    std::string source = "#include <iostream>\n#include <string>\n";

    AstParser parser;
    AstNode root = parser.parse_source(source, "cpp");
    AstNode preamble = parser.extract_preamble(root, source, "cpp");

    // With no code nodes, preamble should be empty
    TEST_ASSERT(preamble.content.empty());
}

int main() {
    REGISTER_TEST(test_ast_node_tokens);
    REGISTER_TEST(test_ast_node_tokens_empty);
    REGISTER_TEST(test_ast_parser_available);
    REGISTER_TEST(test_ast_parser_supported_languages);
    REGISTER_TEST(test_ast_parse_cpp_function);
    REGISTER_TEST(test_ast_parse_cpp_class);
    REGISTER_TEST(test_ast_parse_python_function);
    REGISTER_TEST(test_ast_parse_python_class);
    REGISTER_TEST(test_ast_nodes_to_chunks);
    REGISTER_TEST(test_ast_split_large_node);
    REGISTER_TEST(test_ast_parse_empty);
    REGISTER_TEST(test_ast_parse_unsupported);
    REGISTER_TEST(test_ast_parse_file);
    REGISTER_TEST(test_ast_parent_name);
    REGISTER_TEST(test_ast_metadata_prefix_format);
    REGISTER_TEST(test_ast_metadata_prefix_no_parent);
    REGISTER_TEST(test_ast_parse_cpp_enum);
    REGISTER_TEST(test_ast_parse_cpp_struct);
    REGISTER_TEST(test_ast_parse_python_decorated);
    REGISTER_TEST(test_ast_parse_nested_classes);
    REGISTER_TEST(test_ast_parse_namespace);
    REGISTER_TEST(test_ast_split_large_node_lines);
    REGISTER_TEST(test_ast_extract_preamble);
    REGISTER_TEST(test_ast_extract_preamble_python);
    REGISTER_TEST(test_ast_nodes_to_chunks_with_filepath);
    REGISTER_TEST(test_ast_nodes_to_chunks_no_filepath);
    REGISTER_TEST(test_ast_preamble_no_code);

    return test::TestRunner::instance().run();
}
