// permission_panel.cpp — решение диалога разрешения (И11.8).
//
// Здесь всё, что панель показывает и всё, что она решает. Окно ImGui
// рисует готовые поля (шапка permission_panel.h).

#include "permission_panel.h"

namespace coder {
namespace permission_panel {

std::string headline(const std::string& permission) {
    /* Ключ разрешения — техническое имя ("deploy", "external_directory").
     * Человек о нём не знает, и надпись «Требуется разрешение: deploy»
     * сказала бы ему ровно ноль; ключ всё равно печатается строкой ниже,
     * где он полезен при разборе того, что правило запретило. */
    if (permission.empty()) return "Требуется разрешение";
    return "Требуется разрешение (" + permission + ")";
}

std::string suggest_line(const std::string& suggested) {
    /* Пустой suggested → пустая строка, а не «всегда разрешит: » с
     * ничего не говорящим хвостом. Причина — в шапке: при пустом
     * suggested правило не пишется, и кнопки «Всегда» панель не
     * показывает вовсе. */
    if (suggested.empty()) return std::string();
    return "«Всегда» разрешит: " + suggested;
}

std::string cascade_line(std::size_t others) {
    /* Число названо явно, потому что «Отклонить» без этого обещания
     * тихо убивает ещё N вопросов, которых человек не видел. Формулировка
     * говорит «в том числе», а не «остальные»: каскад бьёт по всем
     * ожидающим (И2.6, permission_engine.cpp), включая те, что придут
     * после клика по этому кадру, — но называть их «будущими» было бы
     * обещанием, которого панель не может сдержать: они могут и не
     * появиться. */
    if (others == 0) return std::string();
    if (others == 1) return "Отклонение отклонит и ещё 1 вопрос:";
    return "Отклонение отклонит и ещё " + std::to_string(others) +
           " вопросов:";
}

Panel build(const std::vector<PermissionRequest>& pending) {
    Panel p;

    /* РЕШЁННЫЕ ЗАПРОСЫ ПРОПУСКАЮТСЯ, и это не косметика.
     *
     * `PermissionEngine::reply` помечает запрос решённым, но НЕ убирает
     * его из очереди: убирает его тот поток агента, который проснётся по
     * condvar и дойдёт до своего `pending_.erase`. Между кликом и этим
     * стиранием проходит несколько кадров окна, и запрос всё это время
     * лежит в очереди с `decided = true`.
     *
     * Без фильтра на экране появлялась бы МЁТРВАЯ панель: кнопки на
     * месте, а `reply` на такой запрос возвращает false (см.
     * permission_engine.cpp) — то есть человек кликает и ничего не
     * происходит. Хуже всего это выглядит после каскадного отклонения:
     * человек отклонил первый вопрос и смотрит на тот же вопрос снова.
     *
     * Фильтр живёт здесь, а не в окне, потому что «показывать решаемое»
     * — это решение о том, что человек увидит, и проверяться оно должно
     * здесь же. */
    const PermissionRequest* first = nullptr;
    std::size_t others = 0;
    for (const PermissionRequest& r : pending) {
        if (r.decided) continue;
        if (!first) {
            first = &r;
        } else {
            ++others;
        }
    }
    if (!first) return p;   /* ждущих ответов нет */

    p.visible = true;

    const PermissionRequest& r = *first;
    p.ask.id = r.id;
    p.ask.permission = r.permission;
    p.ask.metadata = r.metadata;
    p.ask.patterns = r.patterns;
    p.ask.suggested = r.suggested;

    /* Кнопка «Всегда» есть только там, где она что-то записывает.
     * `PermissionEngine::reply` пишет правило лишь при непустом
     * suggested, поэтому при пустом suggested кнопка была бы в точности
     * «Разрешить разово» под другим именем — и человек, нажавший
     * «Всегда», потом удивлялся бы, почему вопрос вернулся. */
    p.ask.can_always = !r.suggested.empty();

    p.ask.headline = headline(r.permission);
    p.ask.once_label = "Разрешить разово";
    p.ask.always_label = "Всегда";
    p.ask.reject_label = "Отклонить";
    p.ask.suggest_line = suggest_line(r.suggested);

    /* Прочие вопросы считаются ЗДЕСЬ, из той же копии очереди, а не
     * повторным снимком движка: два снимка разошлись бы, и подпись
     * назвала бы не то число, которое человек сейчас видит. */
    p.others = others;
    p.ask.cascade_line = cascade_line(p.others);
    return p;
}

}  // namespace permission_panel
}  // namespace coder