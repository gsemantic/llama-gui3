// stream_panel.cpp — решение панели стриминга (И11.6).
//
// Здесь всё, что панель показывает и всё, что она решает. Окно ImGui
// рисует готовые строки и не решает ничего (шапка stream_panel.h).

#include "stream_panel.h"

#include <algorithm>

namespace coder {
namespace stream_panel {

void StreamBuffer::clear() {
    text.clear();
    reasoning.clear();
    tool_open = false;
    tool_call_id.clear();
    tool_name.clear();
    text_open = false;
    reasoning_open = false;
    active = false;
    dropped_chars = 0;
    step = 0;
}

std::size_t StreamBuffer::trim_to(std::string& buf, std::size_t limit) {
if (buf.size() <= limit) return 0;
const std::size_t excess = buf.size() - limit;
    /* Сначала отрезаем РОВНО лишнее, и только потом выравниваем границу.
     * Обратный порядок не обрезал бы ничего: проверка «байт по границе —
     * продолжение?» на позиции 0 смотрит на НАЧАЛО буфера, а не на
     * место реза, и для обычного текста (первый байт никогда не
     * продолжение) цикл не делал ни шага. */
    buf.erase(0, excess);
    /* Граница символа: если первый оставшийся байт — продолжение
     * (0b10xxxxxx), символ разрезан пополам, и его надо дорезать. Иначе
     * начало панели было бы битым, и весь текст до конца читался бы как
     * мусор. */
    std::size_t at = 0;
    while (at < buf.size()
           && (static_cast<unsigned char>(buf[at]) & 0xC0) == 0x80) {
        ++at;
    }
    buf.erase(0, at);
    return excess + at;
}

bool StreamBuffer::push_text(const std::string& delta) {
    if (delta.empty()) return true;
    text += delta;
    text_open = true;
    const std::size_t dropped =
        trim_to(text, limits::kMaxStreamPanelChars);
    dropped_chars += dropped;
    return dropped == 0;
}

bool StreamBuffer::push_reasoning(const std::string& delta) {
    if (delta.empty()) return true;
    reasoning += delta;
    reasoning_open = true;
    /* Размышление обрезается тем же пределом, но в СВОЙ счётчик отброшенных:
     * у него своя строка в панели, и сумма «текст + размышление» в одной
     * цифре сказала бы «потеряно вот столько» без указания где. */
    const std::size_t dropped =
        trim_to(reasoning, limits::kMaxStreamPanelChars);
    dropped_chars += dropped;
    return dropped == 0;
}

void feed(StreamBuffer& buf, const LlmEvent& event) {
    switch (event.kind()) {
        case LlmEventKind::StepStart:
            /* Новый шаг — новая панель. Прежнее содержимое уже дописано в
             * историю циклом, и оставлять его в буфере значило бы показать
             * два хода разом («панель» из текста прошлого шага плюс
             * текущий). */
            buf.clear();
            buf.active = true;
            break;

        case LlmEventKind::TextStart:
            if (!buf.active) buf.active = true;
            buf.text_open = true;
            break;

        case LlmEventKind::TextDelta:
            if (!buf.active) buf.active = true;
            buf.push_text(event.delta());
            break;

        case LlmEventKind::TextEnd:
            /* Закрытие блока, который не открывался, НЕ создаёт блока:
             * провайдер может прислать `text_end` без `text_start`, и
             * панель показала бы пустую строку с закрытым блоком. */
            buf.text_open = false;
            break;

        case LlmEventKind::ReasoningStart:
            if (!buf.active) buf.active = true;
            buf.reasoning_open = true;
            break;

        case LlmEventKind::ReasoningDelta:
            if (!buf.active) buf.active = true;
            buf.push_reasoning(event.delta());
            break;

        case LlmEventKind::ReasoningEnd:
            buf.reasoning_open = false;
            break;

        case LlmEventKind::ToolInputStart:
            if (!buf.active) buf.active = true;
            buf.tool_open = true;
            buf.tool_call_id = event.call_id();
            buf.tool_name = event.tool_name();
            break;

        case LlmEventKind::ToolInputEnd:
            buf.tool_open = false;
            break;

        default:
            break;
    }
}

void finish_turn(StreamBuffer& buf) {
    buf.active = false;
    buf.text_open = false;
    buf.reasoning_open = false;
    buf.tool_open = false;
    buf.tool_call_id.clear();
    buf.tool_name.clear();
}

std::string empty_line() {
    /* Подпись должна объяснять ОТСУТСТВИЕ, а не констатировать его.
     * «ходов ещё не было» после первого хода читается как «панель
     * сломалась», и человек начинает искать несуществующую проблему. */
    return "Модель не печатает: между ходами.";
}

Panel build(const StreamBuffer& buf, const ActiveTool& active_tool) {
    Panel p;
    /* Видимость решается ОДНИМ условием, и это условие — «ходит ли
     * ход». Панель без незакрытого хода не показывается никогда: пустая
     * рамка с надписью занимает место и со временем начинает выглядеть
     * как «что-то сломалось». Пустое состояние объясняет подпись, а не
     * рамка. */
    p.visible = buf.active;

    if (buf.step > 0) {
        p.step_line = "шаг " + std::to_string(buf.step);
    }

    p.text = buf.text;
    p.reasoning = buf.reasoning;
    p.reasoning_open = buf.reasoning_open;
    p.text_open = buf.text_open;
    p.tool_open = buf.tool_open;
    p.tool_call_id = buf.tool_call_id;
    if (!buf.tool_name.empty()) p.tool_name = buf.tool_name;
    p.dropped_chars = buf.dropped_chars;

    /* Активный инструмент показывается, даже когда панели текста нет:
     * ход, в котором модель молча зовёт `bash`, не печатает ничего, и
     * человек видел бы пустую панель при работающем инструменте. */
    if (!active_tool.tool_name.empty()) {
        p.has_tool = true;
        p.tool = active_tool;
        p.visible = true;
    }
    return p;
}

}  // namespace stream_panel
}  // namespace coder