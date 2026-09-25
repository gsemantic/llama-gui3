#pragma once

/*
 * test_support.h — общая подготовка окружения для тестов (И2.5).
 *
 * Тесты не имеют UI, а ToolRunner с И2.5 спрашивает разрешение перед
 * вызовом инструмента и блокирует worker-поток до ответа. Без
 * имитации пользователя тест встал бы на ожидании, которого не
 * будет: это ровно тот висящий GUI, который закрыл дефект D1, только
 * в тестах.
 *
 * approve_all_permissions() — это «пользователь нажал ВСЕГДА на всё».
 * Тесты поведения инструментов (содержимое файла, вывод команды,
 * разбор JSON) не должны проверять разрешения: для этого есть
 * test_permission_engine.cpp, где каждое решение проверяется точечно.
 *
 * Порядок обязателен: сначала Engine::init (он ставит дефолты
 * уровня агента), потом approve. apply_agent_defaults идемпотентна,
 * поэтому повторные init ничего не затирают.
 */

#include "../core/engine.h"
#include "../core/permission_engine.h"

namespace test_support {

inline void approve_all_permissions() {
    coder::engine().permissions().apply_agent_defaults({"/tmp", "/var/tmp"});
    coder::Rule allow_all;
    allow_all.permission = "*";
    allow_all.pattern = "*";
    allow_all.action = coder::PermissionAction::Allow;
    allow_all.comment = "тест: пользователь разрешил всё";
    coder::engine().permissions().add_rule(allow_all);
    /* Исключение: внешние каталоги остаются запрещёнными-спрашиваемыми.
     * Иначе заглушка «пользователь разрешил всё» тихо отключила бы
     * PermissionGate, и тесты гейта на выход за пределы проекта
     * проходили бы, ничего не проверяя. */
    coder::Rule external_still_asks;
    external_still_asks.permission = "external_directory";
    external_still_asks.pattern = "*";
    external_still_asks.action = coder::PermissionAction::Ask;
    external_still_asks.comment = "тест: путь вне проекта по-прежнему спрашивает";
    coder::engine().permissions().add_rule(external_still_asks);
    /* Подстраховка: если где-то всё же останется «спросить», тест
     * получит отказ через 2 секунды и упадёт с внятным сообщением, а не
     * провисит до конца прогона. */
    coder::engine().permissions().set_wait_timeout_ms(2000);
}

} // namespace test_support
