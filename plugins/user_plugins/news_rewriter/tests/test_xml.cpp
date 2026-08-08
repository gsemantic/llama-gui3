#include "test_framework.h"

#include <string>

#include "xml.h"

using namespace news_rewriter;

static void test_parse_rss() {
    const std::string rss =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<rss version=\"2.0\"><channel><title>Test</title>"
        "<item><title>Новость 1</title><link>http://x/1</link>"
        "<description>&lt;p&gt;Текст&lt;/p&gt;</description></item>"
        "<item><title>Новость 2</title><link>http://x/2</link>"
        "<description>Второй текст</description></item>"
        "</channel></rss>";

    XmlNode root;
    TEST_ASSERT_TRUE(parse_xml(rss, root));
    TEST_ASSERT_EQUAL(root.name, "rss");

    const XmlNode* channel = find_child(root, "channel");
    TEST_ASSERT_TRUE(channel != nullptr);
    TEST_ASSERT_EQUAL(child_text(*channel, "title"), "Test");

    const auto items = find_children(*channel, "item");
    TEST_ASSERT_EQUAL(items.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(child_text(*items[0], "title"), "Новость 1");
    TEST_ASSERT_EQUAL(child_text(*items[0], "link"), "http://x/1");
    // Сущности декодированы
    TEST_ASSERT_EQUAL(child_text(*items[0], "description"), "<p>Текст</p>");
    TEST_ASSERT_EQUAL(child_text(*items[1], "title"), "Новость 2");
}

static void test_parse_atom() {
    const std::string atom =
        "<feed xmlns=\"http://www.w3.org/2005/Atom\">"
        "<title>Feed</title>"
        "<entry><title>Entry A</title><link href=\"http://a/1\" rel=\"alternate\"/>"
        "<summary>Sum</summary></entry>"
        "<entry><title>Entry B</title><link href=\"http://a/2\"/></entry>"
        "</feed>";

    XmlNode root;
    TEST_ASSERT_TRUE(parse_xml(atom, root));
    TEST_ASSERT_EQUAL(root.name, "feed");

    const auto entries = find_children(root, "entry");
    TEST_ASSERT_EQUAL(entries.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(child_text(*entries[0], "title"), "Entry A");
    const XmlNode* link = find_child(*entries[0], "link");
    TEST_ASSERT_TRUE(link != nullptr);
    const auto it = link->attrs.find("href");
    TEST_ASSERT_TRUE(it != link->attrs.end());
    TEST_ASSERT_EQUAL(it->second, "http://a/1");
}

static void test_parse_cdata_and_comment() {
    const std::string xml = "<a><!--comment--><![CDATA[text&<keep>]]></a>";
    XmlNode root;
    TEST_ASSERT_TRUE(parse_xml(xml, root));
    TEST_ASSERT_EQUAL(full_text(root), "text&<keep>");
}

static void test_parse_self_closing_and_nested() {
    const std::string xml =
        "<root attr=\"v\"><empty/><nested><child>value</child></nested></root>";
    XmlNode root;
    TEST_ASSERT_TRUE(parse_xml(xml, root));
    TEST_ASSERT_EQUAL(root.name, "root");
    TEST_ASSERT_EQUAL(root.attrs.at("attr"), "v");
    const XmlNode* nested = find_child(root, "nested");
    TEST_ASSERT_TRUE(nested != nullptr);
    TEST_ASSERT_EQUAL(child_text(*nested, "child"), "value");
}

static void test_parse_namespace_local_name() {
    const std::string xml =
        "<rss xmlns:content=\"http://purl.org/rss/1.0/modules/content/\">"
        "<channel><item><title>t</title>"
        "<content:encoded>полный текст</content:encoded></item></channel></rss>";
    XmlNode root;
    TEST_ASSERT_TRUE(parse_xml(xml, root));
    const XmlNode* item = find_child(*find_child(root, "channel"), "item");
    TEST_ASSERT_TRUE(item != nullptr);
    TEST_ASSERT_EQUAL(child_text(*item, "encoded"), "полный текст");
}

static void test_parse_malformed() {
    XmlNode root;
    TEST_ASSERT_FALSE(parse_xml("<a><b></a>", root));
    TEST_ASSERT_FALSE(parse_xml("not xml at all", root));
    TEST_ASSERT_FALSE(parse_xml("", root));
    TEST_ASSERT_FALSE(parse_xml("<a", root));
}

REGISTER_TEST(test_parse_rss);
REGISTER_TEST(test_parse_atom);
REGISTER_TEST(test_parse_cdata_and_comment);
REGISTER_TEST(test_parse_self_closing_and_nested);
REGISTER_TEST(test_parse_namespace_local_name);
REGISTER_TEST(test_parse_malformed);
