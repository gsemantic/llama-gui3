/*
 * test_markdown_renderer.cpp — регрессия на падение generic-токенизации.
 *
 * Code-блок с неизвестным языком (например, ```text из вывода инструмента)
 * уходит в base_tokenize с keywords == nullptr. Прежний is_keyword()
 * разыменовывал kws[0] без проверки: любая строка со словом роняла GUI в
 * tokenize → render_code_block. Здесь проверяется, что generic-разбор
 * возвращает токены и не падает.
 */

#include "../../include/ui/markdown_renderer.h"

#include "../test_framework.h"

using namespace llama_gui::ui;

void test_generic_code_block_tokenizes_words_without_keywords() {
    MarkdownRenderer& r = MarkdownRenderer::instance();

    const std::string line = "read_file -> calculator.py";
    const auto tokens = r.tokenize_for_test(line, "text");
    TEST_ASSERT_TRUE(!tokens.empty());

    /* Каждый токен обязан лежать внутри строки: рендерер делает
     * line.substr(tok.start, tok.end - tok.start), и выход за границу —
     * это чтение чужой памяти. */
    for (const HighlightToken& t : tokens) {
        TEST_ASSERT_TRUE(t.start <= t.end);
        TEST_ASSERT_TRUE(t.end <= line.size());
    }

    /* Пустой язык — тот же generic-путь. */
    const auto empty_lang = r.tokenize_for_test("value = 42", "");
    TEST_ASSERT_TRUE(!empty_lang.empty());
}

void test_unknown_language_falls_back_to_generic() {
    MarkdownRenderer& r = MarkdownRenderer::instance();
    const auto tokens = r.tokenize_for_test("some generic text 123", "no-such-lang");
    TEST_ASSERT_TRUE(!tokens.empty());
}

void test_cyrillic_in_code_block_is_not_split_into_bytes() {
    MarkdownRenderer& r = MarkdownRenderer::instance();
    const std::string line = "[permission] bash: разрешено";
    const auto tokens = r.tokenize_for_test(line, "text");
    TEST_ASSERT_TRUE(!tokens.empty());

    const auto is_boundary = [&line](size_t p) {
        return p == 0 || p == line.size() ||
               (static_cast<unsigned char>(line[p]) & 0xC0) != 0x80;
    };
    for (const HighlightToken& t : tokens) {
        TEST_ASSERT_TRUE(is_boundary(t.start));
        TEST_ASSERT_TRUE(is_boundary(t.end));
    }
}

int main() {
    REGISTER_TEST(test_generic_code_block_tokenizes_words_without_keywords);
    REGISTER_TEST(test_unknown_language_falls_back_to_generic);
    REGISTER_TEST(test_cyrillic_in_code_block_is_not_split_into_bytes);
    return test::TestRunner::instance().run();
}
