#include "ast_parser.h"

// This file provides the tree-sitter language function declarations
// The actual implementations come from the tree-sitter-grammar-* libraries
// linked via CMake.

// These declarations are needed because the tree-sitter grammar libraries
// export C functions, and we need to declare them for the linker.

namespace llama_gui {
namespace core {

// Language initialization is handled by the tree-sitter grammar libraries
// linked at compile time. The language_grammars map in ast_parser.cpp
// uses the tree_sitter_*() functions which are provided by:
// - tree-sitter-grammar-c (tree_sitter_c)
// - tree-sitter-grammar-cpp (tree_sitter_cpp)
// - tree-sitter-grammar-python (tree_sitter_python)
// - tree-sitter-grammar-rust (tree_sitter_rust)

} // namespace core
} // namespace llama_gui
