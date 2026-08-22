#include "test_framework.h"

#include <string>

#include "common.h"
#include "extractor.h"

using namespace news_rewriter;

namespace {

// Автор с маркером «Автор: Имя» в отдельном абзаце: извлекается, тело очищается.
static void test_text_author_russian_marker() {
    const std::string text =
        "Заголовок новости\n\n"
        "Первый абзац основного текста новости, достаточно длинный и содержательный.\n\n"
        "Второй абзац продолжает материал и раскрывает важные детали события.\n\n"
        "Автор: Анна Смирнова";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "Анна Смирнова");
    TEST_ASSERT(body.find("Анна Смирнова") == std::string::npos);
    TEST_ASSERT(body.find("Первый абзац") != std::string::npos);
    TEST_ASSERT(body.find("Второй абзац") != std::string::npos);
}

// Маркер «Автор оригинала: Имя» (составной) тоже распознаётся.
static void test_text_author_original_marker() {
    const std::string text =
        "Текст статьи, единственный абзац с достаточно длинным содержательным текстом.\n\n"
        "Автор оригинала: Иван Петров";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "Иван Петров");
    TEST_ASSERT(body.find("Иван Петров") == std::string::npos);
}

// Латинский маркер «By Имя» (регистронезависимо: «BY», «by»).
static void test_text_author_by_marker() {
    const std::string text =
        "Some sufficiently long news body text that should be kept as the article.\n\n"
        "By John Doe";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "John Doe");
    TEST_ASSERT(body.find("John Doe") == std::string::npos);
}

// Китайский маркер «作者：Имя» с полноширинным двоеточием.
static void test_text_author_cjk_marker() {
    const std::string text =
        "正文内容，一段足够长的材料文字用于 проверки извлечения.\n\n"
        "作者：赵广立";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "赵广立");
    TEST_ASSERT(body.find("赵广立") == std::string::npos);
}

// Маркер без имени в своей строке → имя берётся из следующего абзаца.
static void test_text_author_marker_then_name_paragraph() {
    const std::string text =
        "Тело статьи, достаточно длинное и содержательное для проверки.\n\n"
        "Автор:\n\n"
        "Пётр Иванов";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "Пётр Иванов");
    TEST_ASSERT(body.find("Автор") == std::string::npos);
    TEST_ASSERT(body.find("Пётр Иванов") == std::string::npos);
}

// Без маркера: последний абзац — короткое ФИО, извлекается как имя.
static void test_text_author_name_only_last_paragraph() {
    const std::string text =
        "Первый абзац новости, довольно длинный и содержательный текст.\n\n"
        "Второй абзац с важными деталями и подробностями произошедшего.\n\n"
        "Мария Сидорова";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "Мария Сидорова");
    TEST_ASSERT(body.find("Мария Сидорова") == std::string::npos);
}

// Инициалы: «И. И. Иванов» в последнем абзаце распознаются как имя.
static void test_text_author_initials_last_paragraph() {
    const std::string text =
        "Текст новости достаточно длинный, чтобы считаться телом статьи.\n\n"
        "И. И. Иванов";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "И. И. Иванов");
}

// Если автора нет (последний абзац — обычное предложение), возвращается false,
// а тело остаётся равным исходному тексту.
static void test_text_author_absent_returns_false() {
    const std::string text =
        "Первый абзац новости, довольно длинный и содержательный текст.\n\n"
        "Второй абзац с важными деталями произошедшего события в городе.";
    std::string author, body;
    TEST_ASSERT_FALSE(extract_author_from_text(text, author, body));
    TEST_ASSERT_TRUE(author.empty());
    TEST_ASSERT_EQUAL(body, text);
}

// Автор внутри абзаца (НЕ отдельным параграфом) не извлекается: требование —
// имя должно быть в отдельном абзаце.
static void test_text_author_not_separate_paragraph_is_ignored() {
    const std::string text =
        "Первый абзац новости, длинный и содержательный текст.\n\n"
        "Второй абзац. Автор: Сергей Волков";
    std::string author, body;
    TEST_ASSERT_FALSE(extract_author_from_text(text, author, body));
    TEST_ASSERT_TRUE(author.empty());
}

// Переводы строк \r\n нормализуются, автор в отдельном абзаце извлекается.
static void test_text_author_crlf() {
    const std::string text =
        "Текст статьи, достаточно длинный и содержательный для проверки.\r\n\r\n"
        "Автор: Олег Кузнецов";
    std::string author, body;
    TEST_ASSERT_TRUE(extract_author_from_text(text, author, body));
    TEST_ASSERT_EQUAL(author, "Олег Кузнецов");
    TEST_ASSERT(body.find("Олег Кузнецов") == std::string::npos);
}

REGISTER_TEST(test_text_author_russian_marker);
REGISTER_TEST(test_text_author_original_marker);
REGISTER_TEST(test_text_author_by_marker);
REGISTER_TEST(test_text_author_cjk_marker);
REGISTER_TEST(test_text_author_marker_then_name_paragraph);
REGISTER_TEST(test_text_author_name_only_last_paragraph);
REGISTER_TEST(test_text_author_initials_last_paragraph);
REGISTER_TEST(test_text_author_absent_returns_false);
REGISTER_TEST(test_text_author_not_separate_paragraph_is_ignored);
REGISTER_TEST(test_text_author_crlf);

} // namespace
