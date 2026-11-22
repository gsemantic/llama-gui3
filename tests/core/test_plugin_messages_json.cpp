/*
 * test_plugin_messages_json.cpp — граница ABI между хостом и плагином:
 * разбор messages_json в реплики (мультимодальность).
 *
 * Почему это проверяется отдельно, а не «вместе с чатом»: на этой границе
 * вложения и терялись. Плагин отправляет картинку в структуре реплики,
 * хост её разбирает — и если разбор молча выбрасывает незнакомое, ход
 * уходит модели без изображения, а провайдер отвечает «не вижу картинки».
 * Дефект при этом не виден НИГДЕ: запрос ушёл, ответ пришёл, и только
 * сравнение с ожидаемым показывает, что половина реплики пропала.
 *
 * Проверяется ровно то, что может испортиться молча:
 *   - текстовая реплика остаётся строкой (иначе сломали бы всё);
 *   - реплика с картинкой разбирается в images, а не теряется;
 *   - нераспознанная часть — ОТКАЗ, а не тихий пропуск (тихий пропуск
 *     выглядел бы как «модель не разглядела» и уводил бы поиск в модель);
 *   - несколько текстовых частей склеиваются, а не затирают друг друга.
 */

#include "../../include/plugins/plugin_manager.h"
#include "../test_framework.h"

#include <string>
#include <vector>

using namespace llama_gui::plugin;
using llama_gui::core::ChatMessage;
using llama_gui::core::MessageRole;

namespace {

/* Короткая форма проверки «разбор удался»: error не должен остаться
 * заполненным от прошлого вызова — он передаётся по ссылке и вызывающий
 * обязан считать пустую строку успехом. */
bool parses(const std::string& json, std::vector<ChatMessage>& out,
            std::string& err) {
    err.clear();
    return parse_messages_json(json.c_str(), out, err);
}

} // namespace

/* Текстовая реплика уезжает строкой — регрессионный страж на «сломали
 * текстовый путь, доехав до мультимодальности». */
void text_message_parses_as_before() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(R"([{"role":"user","content":"привет"}])", out, err));
    TEST_ASSERT_EQUAL(out.size(), size_t(1));
    TEST_ASSERT_EQUAL(out[0].content, std::string("привет"));
    TEST_ASSERT_EQUAL(out[0].images.size(), size_t(0));
}

/* Роль ассистента не превращается в пользовательскую: разбор реплики и
 * присвоение роли — разные шаги, и перепутать их значило бы отдать модели
 * её собственные слова как слова человека. */
void roles_are_preserved() {
    std::vector<ChatMessage> out;
    std::string err;
    /* В JSON обязана быть ЗАКРЫВАЮЩАЯ скоба массива: без неё разбор
     * законно падает, и проверка «роли сохраняются» падала бы на
     * собственном опечаточном литерале, а не на коде. */
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"system","content":"s"},{"role":"assistant","content":"a"}])",
        out, err));
    TEST_ASSERT_EQUAL(out.size(), size_t(2));
    TEST_ASSERT_TRUE(out[0].role == MessageRole::System);
    TEST_ASSERT_TRUE(out[1].role == MessageRole::Assistant);
}

/* РОЛЬ ГЛАВНОЕ: реплика с изображением разбирается, а картинка доезжает
 * до ChatMessage::images. Это и есть тот дефект — «модель не видит
 * вложение, хотя мультимодальная». */
void image_part_reaches_the_message() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"user","content":[{"type":"text","text":"что здесь не так?"},)"
        R"({"type":"image_url","image_url":{"url":"data:image/png;base64,AAAA",)"
        R"("media_type":"image/png"}}]}])",
        out, err));

    TEST_ASSERT_EQUAL(out.size(), size_t(1));
    TEST_ASSERT_EQUAL(out[0].content, std::string("что здесь не так?"));
    TEST_ASSERT_EQUAL(out[0].images.size(), size_t(1));
    TEST_ASSERT_EQUAL(out[0].images[0].url,
                      std::string("data:image/png;base64,AAAA"));
    TEST_ASSERT_EQUAL(out[0].images[0].media_type, std::string("image/png"));
}

/* Реплика из ОДНОЙ картинки, без текста: законный формат, и content
 * остаётся пустым, а не «что-нибудь». */
void image_without_text_keeps_an_empty_content() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"user","content":[{"type":"image_url","image_url":)"
        R"({"url":"data:image/jpeg;base64,BBBB"}}]}])",
        out, err));

    TEST_ASSERT_EQUAL(out.size(), size_t(1));
    TEST_ASSERT_TRUE(out[0].content.empty());
    TEST_ASSERT_EQUAL(out[0].images.size(), size_t(1));
}

/* Две картинки сохраняют порядок: порядок вложений человек видит в списке
 * под полем ввода, и он же определяет порядок частей у провайдера. */
void multiple_images_keep_their_order() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"user","content":[{"type":"image_url","image_url":{"url":"A"}},)"
        R"({"type":"image_url","image_url":{"url":"B"}}]}])",
        out, err));

    TEST_ASSERT_EQUAL(out[0].images.size(), size_t(2));
    TEST_ASSERT_EQUAL(out[0].images[0].url, std::string("A"));
    TEST_ASSERT_EQUAL(out[0].images[1].url, std::string("B"));
}

/* НЕРАСПОЗНАННАЯ часть — ОТКАЗ, а не тихий пропуск.
 *
 * Тихий пропуск выглядел бы как «модель не разглядела картинку», и причину
 * искали бы в модели. Отказ виден сразу и называет тип. */
void unknown_part_type_is_an_error_not_a_silent_drop() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_FALSE(parses(
        R"([{"role":"user","content":[{"type":"video_url","video_url":{"url":"x"}}]}])",
        out, err));
    TEST_ASSERT_TRUE(err.find("video_url") != std::string::npos);
}

/* Часть-картинка БЕЗ url — тоже отказ: `{"url":""}` уехало бы провайдеру
 * как «изображение», которого нет, и ответ «не вижу картинки» приписывали
 * бы модели. */
void image_part_without_url_is_an_error() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_FALSE(parses(
        R"([{"role":"user","content":[{"type":"image_url","image_url":{}}]}])",
        out, err));
    TEST_ASSERT_TRUE(err.find("url") != std::string::npos);
}

/* Несколько текстовых частей склеиваются, а не затирают друг друга.
 *
 * Провайдеры, разбивающие ответ на куски, так и делают, и `=` тихо выбросил
 * бы все, кроме последней: запрос ушёл бы, модель ответила бы, и только
 * сравнение с исходником показало бы, что половина текста пропала. */
void multiple_text_parts_are_joined_not_overwritten() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"user","content":[{"type":"text","text":"раз"},)"
        R"({"type":"text","text":"два"}]}])",
        out, err));

    TEST_ASSERT_EQUAL(out.size(), size_t(1));
    TEST_ASSERT_TRUE(out[0].content.find("раз") != std::string::npos);
    TEST_ASSERT_TRUE(out[0].content.find("два") != std::string::npos);
}

/* Короткая форма image_url — просто строкой. Так пишут некоторые шлюзы, и
 * отвергнутая картинка выглядела бы как «модель слепая». */
void image_url_as_a_plain_string_is_accepted() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"user","content":[{"type":"image_url","image_url":"data:image/png;base64,CCCC"}]}])",
        out, err));

    TEST_ASSERT_EQUAL(out[0].images.size(), size_t(1));
    TEST_ASSERT_EQUAL(out[0].images[0].url,
                      std::string("data:image/png;base64,CCCC"));
    /* media_type не указан — остаётся пустым, и в запрос не уезжает. */
    TEST_ASSERT_TRUE(out[0].images[0].media_type.empty());
}

/* Мусор во входе — отказ с причиной, а не пустой список реплик: пустой
 * список выглядел бы как «человек ничего не написал». */
void garbage_input_is_an_error() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_FALSE(parse_messages_json(nullptr, out, err));
    TEST_ASSERT_TRUE(!err.empty());

    out.clear();
    TEST_ASSERT_FALSE(parse_messages_json("{не json", out, err));
    TEST_ASSERT_TRUE(!err.empty());

    /* Объект вместо массива. */
    out.clear();
    TEST_ASSERT_FALSE(parse_messages_json(R"({"role":"user"})", out, err));
    TEST_ASSERT_TRUE(!err.empty());

    /* Пустой массив. */
    out.clear();
    TEST_ASSERT_FALSE(parse_messages_json("[]", out, err));
    TEST_ASSERT_TRUE(!err.empty());
}

/* Несколько реплик в одном запросе: история разбирается целиком и в
 * порядке, иначе модель получала бы ходы в перепутанном виде. */
void history_order_is_preserved() {
    std::vector<ChatMessage> out;
    std::string err;
    TEST_ASSERT_TRUE(parses(
        R"([{"role":"user","content":"первый"},)"
        R"({"role":"assistant","content":"ответ"},)"
        R"({"role":"user","content":"второй"}])",
        out, err));

    TEST_ASSERT_EQUAL(out.size(), size_t(3));
    TEST_ASSERT_EQUAL(out[0].content, std::string("первый"));
    TEST_ASSERT_EQUAL(out[1].content, std::string("ответ"));
    TEST_ASSERT_EQUAL(out[2].content, std::string("второй"));
}

int main() {
    REGISTER_TEST(text_message_parses_as_before);
    REGISTER_TEST(roles_are_preserved);
    REGISTER_TEST(image_part_reaches_the_message);
    REGISTER_TEST(image_without_text_keeps_an_empty_content);
    REGISTER_TEST(multiple_images_keep_their_order);
    REGISTER_TEST(unknown_part_type_is_an_error_not_a_silent_drop);
    REGISTER_TEST(image_part_without_url_is_an_error);
    REGISTER_TEST(multiple_text_parts_are_joined_not_overwritten);
    REGISTER_TEST(image_url_as_a_plain_string_is_accepted);
    REGISTER_TEST(garbage_input_is_an_error);
    REGISTER_TEST(history_order_is_preserved);

    return test::TestRunner::instance().run();
}