/*
 * test_stream_panel.cpp — панель стриминга (И11.6).
 *
 * Проверяется не «дельта пришла», а то, ЧТО человек увидит: панель
 * показывает незакрытый ход, режет текст с начала и говорит об этом,
 * держит размышление отдельно от текста и показывает активный
 * инструмент даже тогда, когда модель молчит.
 *
 * Граница с таймлайном (И11.3–11.5) проверяется здесь же, потому что
 * обе панели показывают работу агента и разъехаться могут молча:
 * закрытый ход в панели стриминга выглядел бы как «модель печатает
 * второй раз то, что уже показано в дереве».
 */

#include "core/stream_panel.h"

#include "core/limits.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

using namespace coder;
using namespace coder::stream_panel;

namespace {

/* Разыгранный кусок хода: открыли блок, напечатали, закрыли. */
void emit_text(StreamBuffer& buf, const std::string& text) {
    feed(buf, LlmEvent::text_start());
    feed(buf, LlmEvent::text_delta(text));
    feed(buf, LlmEvent::text_end());
}

void start_turn(StreamBuffer& buf) {
    feed(buf, LlmEvent::step_start());
}

}  // namespace

/* --- Видимость: панель есть только у незакрытого хода --- */

TEST(панель_не_видна_у_пустого_буфера) {
    const Panel p = build(StreamBuffer(), ActiveTool());
    ASSERT_FALSE(p.visible);
}

TEST(панель_видна_пока_ход_не_закрыт) {
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "думаю");

    ASSERT_TRUE(build(buf, ActiveTool()).visible);
}

TEST(панель_гаснет_когда_ход_закрыт) {
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "думаю");
    finish_turn(buf);

    /* Содержимое НЕ стирается — ход ещё не дописан в историю, и буфер
     * остаётся единственным местом, где текст виден. Гаснет только
     * признак активности: панель перестаёт показываться. */
    const Panel p = build(buf, ActiveTool());
    ASSERT_FALSE(p.visible);
    ASSERT_EQ(p.text, std::string("думаю"));
}

TEST(следующий_шаг_сбрасывает_буфер) {
    /* Два хода разом в одной панели читались бы как «модель печатает всё
     * сразу», а текст прошлого шага к этому моменту уже в истории. */
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "прошлый шаг");
    finish_turn(buf);

    start_turn(buf);
    emit_text(buf, "новый шаг");

    const Panel p = build(buf, ActiveTool());
    ASSERT_EQ(p.text, std::string("новый шаг"));
}

/* --- Текст и размышление раздельно --- */

TEST(размышление_не_смешивается_с_текстом) {
    /* Человек читал бы мысли модели как ответ. Раздельно — значит
     * раздельно в ДАННЫХ, а не только на экране. */
    StreamBuffer buf;
    start_turn(buf);
    feed(buf, LlmEvent::reasoning_start());
    feed(buf, LlmEvent::reasoning_delta("размышляю"));
    feed(buf, LlmEvent::reasoning_end());
    emit_text(buf, "ответ");

    const Panel p = build(buf, ActiveTool());
    ASSERT_EQ(p.reasoning, std::string("размышляю"));
    ASSERT_EQ(p.text, std::string("ответ"));
    ASSERT_TRUE(p.reasoning.find("ответ") == std::string::npos);
    ASSERT_TRUE(p.text.find("размышляю") == std::string::npos);
}

TEST(размышление_и_текст_не_смешиваются_порядком_прихода) {
    /* Порядок прихода не должен влиять на раздельность: размышление
     * после текста — обычное дело у моделей с «размышлением». */
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "ответ");
    feed(buf, LlmEvent::reasoning_start());
    feed(buf, LlmEvent::reasoning_delta("поздняя мысль"));

    const Panel p = build(buf, ActiveTool());
    ASSERT_EQ(p.text, std::string("ответ"));
    ASSERT_EQ(p.reasoning, std::string("поздняя мысль"));
}

/* --- «Печатает» против «дописал и молчит» --- */

TEST(печать_и_покой_различаются) {
    /* По этому признаку рисуется курсор. Если бы панель показывала
     * «печатает» и у закрытого блока, человек смотрел бы на вечный
     * курсор и ждал бы текста, который не придёт. */
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "готово");

    ASSERT_FALSE(build(buf, ActiveTool()).text_open);

    feed(buf, LlmEvent::text_start());
    feed(buf, LlmEvent::text_delta("пишу"));
    ASSERT_TRUE(build(buf, ActiveTool()).text_open);
}

/* --- Закрытие несуществующего блока --- */

TEST(конец_блока_без_начала_не_создаёт_пустую_строку) {
    /* Провайдер может прислать text_end без text_start. Панель показала
     * бы пустую строку с закрытым блоком — «модель напечатала пустое». */
    StreamBuffer buf;
    start_turn(buf);
    feed(buf, LlmEvent::text_end());

    ASSERT_FALSE(build(buf, ActiveTool()).text_open);
    ASSERT_EQ(build(buf, ActiveTool()).text, std::string());
}

/* --- Предел: обрезка с начала и честная потеря --- */

TEST(переполнение_обрезает_с_начала_и_считает_потерю) {
    /* Обрезка с конца отбросила бы самое интересное (конец хода), а
     * молчаливая обрезка выглядела бы как «модель так начала говорить».
     * Текст с обрезкой поэтому показывается С НАЧАЛА, а потеря — числом. */
    StreamBuffer buf;
    start_turn(buf);
    for (int i = 0; i < 200; ++i) {
        feed(buf, LlmEvent::text_delta(std::string(100, 'x')));
    }

    const Panel p = build(buf, ActiveTool());
    ASSERT_TRUE(p.text.size() <= limits::kMaxStreamPanelChars);
    ASSERT_TRUE(p.dropped_chars > 0);
    /* Потеря названа ровно настолько, насколько и отброшено. */
    ASSERT_TRUE(p.dropped_chars + p.text.size() >= 20000 - 100);
}

TEST(обрезка_не_режет_многобайтовый_символ) {
    /* Срез серединой UTF-8 оставил бы битый байт в начале, и весь текст
     * до конца читался бы как мусор.
     *
     * Подстановка подобрана ТАК, чтобы граница реза падала ВНУТРЬ
     * символа, а не рядом с ним: символ занимает три байта, хвост
     * дописан так, что `excess = size - limit` указывает на СРЕДНИЙ байт
     * (0x9C — продолжение). Первая версия проверки ставила символ в
     * конец и получала на границе начало символа, то есть реза НЕ
     * попадала внутрь, и проверка была зелёной на любом коде — включая
     * тот, что режет символы. Нашёл это мутационный прогон: «обрезка не
     * выравнивает границу» выжила. */
    StreamBuffer buf;
    start_turn(buf);
    /* Символ В НАЧАЛЕ, хвост после: тогда граница реза (excess = 1)
     * указывает на ВТОРОЙ байт символа, и реза попадает внутрь него.
     * Обратный порядок — символ в конце — давал на границе обычный
     * 'a', то есть реза проходила мимо символа, и проверка была зелёной
     * на любом коде, включая тот, что режет символы. */
    std::string payload = "\xE2\x9C\x93";  /* «✓», три байта, в начале */
    payload.append(limits::kMaxStreamPanelChars - 2, 'a');
    feed(buf, LlmEvent::text_delta(payload));

    const Panel p = build(buf, ActiveTool());
    ASSERT_TRUE(p.text.size() <= limits::kMaxStreamPanelChars);
    /* Начало не может быть продолжением символа — это и есть «граница
     * символа». Только начало: байты продолжения в середине текста
     * законны, и проверка «продолжений нет вообще» ловила бы не дефект
     * обрезки, а нормальный многобайтовый текст. */
    ASSERT_FALSE(p.text.empty());
    const unsigned char first = static_cast<unsigned char>(p.text[0]);
    ASSERT_TRUE((first & 0xC0) != 0x80);
    /* Символ у начала не разрезан: за ним не должно тянуться хвост из
     * продолжений длиннее, чем допускает UTF-8. */
    std::size_t cont = 0;
    for (std::size_t i = 1; i < p.text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(p.text[i]);
        if ((c & 0xC0) != 0x80) break;
        ++cont;
        ASSERT_TRUE(cont < 4);
    }
}

TEST(размышление_обрезается_отдельно_от_текста) {
    /* Предел общий, но обрезка каждого своя: иначе длинное размышление
     * съедало бы текст ответа, который человек как раз и ждёт. */
    StreamBuffer buf;
    start_turn(buf);
    feed(buf, LlmEvent::reasoning_start());
    for (int i = 0; i < 200; ++i) {
        feed(buf, LlmEvent::reasoning_delta(std::string(100, 'r')));
    }
    emit_text(buf, "ответ");

    const Panel p = build(buf, ActiveTool());
    ASSERT_EQ(p.text, std::string("ответ"));
    ASSERT_TRUE(p.reasoning.size() <= limits::kMaxStreamPanelChars);
}

/* --- Активный инструмент --- */

TEST(активный_инструмент_виден_даже_когда_модель_молчит) {
    /* Ход, в котором модель молча зовёт `bash`, не печатает ничего.
     * Без этого пустая панель при работающем инструменте выглядела бы
     * как «агент завис». */
    StreamBuffer buf;
    start_turn(buf);
    ActiveTool tool;
    tool.tool_name = "bash";
    tool.call_id = "call_1";
    tool.elapsed_ms = 1500;

    const Panel p = build(buf, tool);
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(p.has_tool);
    ASSERT_EQ(p.tool.tool_name, std::string("bash"));
}

TEST(без_инструмента_панель_о_tool_молчит) {
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "текст");

    const Panel p = build(buf, ActiveTool());
    ASSERT_FALSE(p.has_tool);
}

TEST(инструмент_виден_даже_при_пустом_буфере) {
    /* Инструмент работает ДО того, как модель что-то напечатала: цикл
     * запускает его по завершении хода. Панель без буфера, но с
     * инструментом — обычное состояние, и пустой рамки быть не должно. */
    const Panel p = build(StreamBuffer(), ActiveTool{"call_1", "read_file", 10});
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(p.has_tool);
}

/* --- Ввод аргументов вызова --- */

TEST(пишет_аргументы_вызова_их_видно) {
    StreamBuffer buf;
    start_turn(buf);
    feed(buf, LlmEvent::tool_input_start("call_9", "write_file"));
    feed(buf, LlmEvent::tool_input_delta("call_9", "{\"path\":"));

    const Panel p = build(buf, ActiveTool());
    ASSERT_TRUE(p.tool_open);
    ASSERT_EQ(p.tool_call_id, std::string("call_9"));
    ASSERT_EQ(p.tool_name, std::string("write_file"));
}

TEST(закрытый_ввод_вызова_больше_не_пишет) {
    StreamBuffer buf;
    start_turn(buf);
    feed(buf, LlmEvent::tool_input_start("call_9", "write_file"));
    feed(buf, LlmEvent::tool_input_end("call_9"));

    ASSERT_FALSE(build(buf, ActiveTool()).tool_open);
}

/* --- Подпись шага и пустое состояние --- */

TEST(шаг_подписан_своим_номером) {
    StreamBuffer buf;
    start_turn(buf);
    buf.step = 3;
    emit_text(buf, "текст");

    ASSERT_EQ(build(buf, ActiveTool()).step_line, std::string("шаг 3"));
}

TEST(без_шага_подпись_пуста_а_не_врёт) {
    /* «шаг 0» — это утверждение о шаге, которого не было. */
    StreamBuffer buf;
    start_turn(buf);
    emit_text(buf, "текст");

    ASSERT_EQ(build(buf, ActiveTool()).step_line, std::string());
}

TEST(пустое_состояние_объясняет_отсутствие) {
    /* Подпись должна объяснять, а не констатировать: «ходов ещё не
     * было» после первого хода читается как «панель сломалась». */
    const std::string line = empty_line();
    ASSERT_FALSE(line.empty());
    ASSERT_TRUE(line.find("не печатает") != std::string::npos);
}
/* --- Граница с окном: окно рисует, но не решает --- */

TEST(окно_берёт_решение_панели_из_core) {
    /* Окно ImGui юнит-тестом не проверяется, поэтому решение «что видно»
     * обязано лежать в core. Собственное условие показа в окне стало бы
     * непроверяемым, и расхождение с core выглядело бы как «панель
     * иногда не появляется».
     *
     * Проверяются ДВА утверждения, и оба mechanical:
     *   1. функция рисования зовёт `stream_panel::build` — то есть решение
     *      действительно приходит из core, а не собирается на месте;
     *   2. выход по невидимости стоит ДО любой отрисовки содержимого —
     *      то есть окно не рисует ничего, когда core решил, что панели
     *      нет. Обратный порядок дал бы пустую рамку с надписью.
     *
     * Что окно решает РАЗРЕШЕНО — это пуст ли блок текста перед его
     * отрисовкой (иначе была бы строка из одного курсора). Проверка
     * границей «флаг виден → первая отрисовка» это допускает и не
     * ломается на таком случае. */
    std::ifstream f(fs::path(__FILE__).parent_path().parent_path() /
                        "ui/coder_window.cpp",
                    std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string ui = ss.str();
    ASSERT_FALSE(ui.empty());

    const std::size_t at = ui.find("static void render_stream_panel()");
    ASSERT_TRUE(at != std::string::npos);
    const std::size_t end = ui.find("\nstatic ", at + 10);
    const std::string body =
        ui.substr(at, end == std::string::npos ? std::string::npos : end - at);

    /* (1) решение пришло из core. */
    ASSERT_TRUE(body.find("stream_panel::build(") != std::string::npos);

    /* (2) выход по невидимости ДО первой отрисовки. */
    const std::size_t gate = body.find("if (!panel.visible) return;");
    ASSERT_TRUE(gate != std::string::npos);
    const std::size_t first_draw = body.find("ImGui::TreeNodeEx");
    ASSERT_TRUE(first_draw != std::string::npos);
    ASSERT_TRUE(gate < first_draw);
}
