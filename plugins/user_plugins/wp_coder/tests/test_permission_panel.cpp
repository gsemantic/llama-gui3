/*
 * test_permission_panel.cpp — диалог разрешения (И11.8).
 *
 * Проверсяется не «кнопка нарисована», а то, что человек получит от
 * панели: что покажет вопрос, ЧТО будет с остальными вопросами при
 * отказе и — главное — что кнопка «Всегда» не появляется там, где
 * «всегда» не бывает.
 *
 * Механизм (PermissionEngine) проверяется отдельно, в
 * test_permission_engine.cpp. Здесь он нужен как ЖИВОЙ: панель собирается
 * из настоящей очереди, потому что её изъян — кнопка «Всегда», под которой
 * не пишется правило, — виден только в связке с `reply`, а не на
 * изолированной структуре.
 */

#include "core/permission_panel.h"

#include "core/engine.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace coder;
using namespace coder::permission_panel;

namespace {

/* Вопрос из очереди — ровно та форма, которую кладёт PermissionEngine.
 * patterns передаётся списком, а не строкой: разбирать «|» в тестовой
 * фикстуре значило бы проверять разбор, которого в панели нет. */
PermissionRequest req(uint64_t id, const char* permission,
                      std::vector<std::string> patterns,
                      const char* suggested, const char* metadata) {
    PermissionRequest r;
    r.id = id;
    r.permission = permission;
    r.patterns = std::move(patterns);
    r.suggested = suggested;
    r.metadata = metadata;
    return r;
}

/* Типовой вопрос: команда, предложен «всегда» на безопасный префикс. */
PermissionRequest bash_req(uint64_t id = 1) {
    return req(id, "bash", {"git status"}, "git status*", "bash → git status");
}

/* Вопрос БЕЗ предложенного паттерна — как у doom_loop. */
PermissionRequest doom_req(uint64_t id = 2) {
    return req(id, "doom_loop", {"bash"}, "",
               "Зацикливание: bash");
}

/* Разрешения — синглтон движка, состояние между проверками протекает. */
struct PermGuard {
    PermGuard() {
        engine().permissions().reset();
        engine().permissions().set_wait_timeout_ms(0);
    }
    ~PermGuard() {
        engine().permissions().cancel_all();
        engine().permissions().reset();
    }
};

/* Дождаться, пока в очереди появится ровно n вопросов. */
bool wait_pending(std::size_t n) {
    for (int i = 0; i < 400; ++i) {
        if (engine().permissions().pending_count() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/* Вырезать комментарии из куска исходника.
 *
 * Без этого проверка «в окне нет решения» ловила бы СЛОВА В КОММЕНТАРИИ:
 * комментарий про то, почему кнопки «Всегда» нет на doom_loop, содержит
 * «Разрешить разово» — и проверка падала бы на правильном коде. Хуже
 * того, её пришлось бы «чинить», убирая из комментария нужное слово, а
 * не исправляя решение. Проверка ищет решение в КОДЕ, поэтому и код
 * должен быть без комментариев. */
std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_line = false;
    bool in_block = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (in_line) {
            if (c == '\n') { in_line = false; out += c; }
            continue;
        }
        if (in_block) {
            if (c == '*' && n == '/') { in_block = false; ++i; }
            continue;
        }
        if (c == '/' && n == '/') { in_line = true; ++i; continue; }
        if (c == '/' && n == '*') { in_block = true; ++i; continue; }
        out += c;
    }
    return out;
}

}  // namespace

/* --- Видимость --- */

TEST(пустая_очередь_не_показывает_панель) {
    /* Рамка с заголовком «Требуется разрешение» и нулём вопросов
     * выглядела бы как «агент сломан и ждёт ответа, которого нет». */
    const Panel p = build(std::vector<PermissionRequest>());
    ASSERT_FALSE(p.visible);
}

TEST(один_вопрос_показывает_панель) {
    const Panel p = build({bash_req()});
    ASSERT_TRUE(p.visible);
    ASSERT_EQ(p.ask.id, (uint64_t)1);
    ASSERT_TRUE(p.ask.permission == "bash");
    ASSERT_TRUE(p.ask.metadata == "bash → git status");
    ASSERT_EQ(p.ask.patterns.size(), (size_t)1);
    ASSERT_TRUE(p.ask.patterns[0] == "git status");
}

/* --- Кнопка «Всегда»: главное решение панели --- */

TEST(кнопка_всегда_есть_когда_есть_что_записать) {
    const Panel p = build({bash_req()});
    ASSERT_TRUE(p.ask.can_always);
    /* Подпись обязана называть, ЧТО именно будет разрешено навсегда:
     * «всегда» без значения — обещание, которое нельзя проверить. */
    ASSERT_TRUE(p.ask.suggest_line.find("git status*") != std::string::npos);
}

TEST(кнопки_всегда_нет_когда_предложить_нечего) {
    /* Настоящий дефект, а не гипотеза: `PermissionEngine::reply` пишет
     * правило только при непустом suggested, поэтому у doom_loop кнопка
     * «Всегда» делала РОВНО то же, что «Разрешить разово». Человек,
     * нажавший её, получал вопрос снова и не понимал почему. */
    const Panel p = build({doom_req()});
    ASSERT_FALSE(p.ask.can_always);
    /* Подписи обещания быть не должно — иначе она осталась бы на экране
     * без кнопки, которая её выполняет. */
    ASSERT_TRUE(p.ask.suggest_line.empty());
}

TEST(пустой_вопрос_без_паттерна_всё_равно_показывает_панель) {
    /* Пустой suggested — не пустой вопрос. Панель обязана показать
     * «Разрешить разово» и «Отклонить»: агент ждёт, и молчать нельзя. */
    const Panel p = build({doom_req()});
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(p.ask.once_label == "Разрешить разово");
    ASSERT_TRUE(p.ask.reject_label == "Отклонить");
}

/* --- Каскад отклонения: человек должен знать до нажатия --- */

TEST(один_вопрос_не_пугает_каскадом) {
    /* «Отклонит ещё 0 вопросов» — пугать нулём нельзя: слово появилось
     * бы у каждого одиночного вопроса и значило бы ровно ничего. */
    const Panel p = build({bash_req()});
    ASSERT_EQ(p.others, (std::size_t)0);
    ASSERT_TRUE(p.ask.cascade_line.empty());
}

TEST(каскад_называет_число_остальных_вопросов) {
    const Panel p = build({bash_req(1), doom_req(2), bash_req(3)});
    ASSERT_EQ(p.others, (std::size_t)2);
    /* Число обязано быть В ПОДПИСИ, а не только в счётчике: человек
     * смотрит на экран, а не на структуру. */
    ASSERT_TRUE(p.ask.cascade_line.find("2") != std::string::npos);
    ASSERT_TRUE(p.ask.cascade_line.find("Отклон") != std::string::npos);
}

TEST(каскад_считает_одного_остального_по_единственному) {
    /* «1 вопросов» — грамматическая нелепость, из-за которой подпись
     * перестаёт читаться как сообщение. */
    const Panel p = build({bash_req(1), doom_req(2)});
    ASSERT_EQ(p.others, (std::size_t)1);
    ASSERT_TRUE(p.ask.cascade_line.find("1 вопрос") != std::string::npos);
    ASSERT_TRUE(p.ask.cascade_line.find("вопросов") == std::string::npos);
}

TEST(показывается_первый_а_не_последний) {
    /* Первым показывается вопрос, на который человек и отвечает, и ответ
     * идёт по ЕГО id: если бы панель показала чужой, кнопка встала бы не
     * туда и отвечала бы за другой вопрос.
     *
     * id здесь различаются, поэтому проверка видит выбор, а не «что-то
     * показалось». Первая версия проверки брала очередь из одинаковых
     * вопросов и тем самым была зелёной на любом коде — поймала
     * мутация. */
    const Panel p = build({bash_req(11), doom_req(22), bash_req(33)});
    ASSERT_EQ(p.ask.id, (uint64_t)11);
    ASSERT_TRUE(p.ask.permission == "bash");
    /* И число прочих не сходится с «показан последний»: у последнего
     * прочих было бы 0. */
    ASSERT_EQ(p.others, (std::size_t)2);
}

TEST(решённый_вопрос_пропускается_а_не_показывается) {
    /* Решённый запрос лежит в очереди до тех пор, пока проснётся поток
     * агента и вычистит его. Показать его — значит показать панель,
     * кнопки которой ничего не делают: `reply` на decided возвращает
     * false. Хуже всего это выглядит после каскадного отклонения — человек
     * отклонил первый вопрос и смотрит на тот же вопрос снова.
     *
     * Решённый стоит ПЕРВЫМ: пока он был вторым, проверка оставалась
     * зелёной и при снятом фильтре — первым всё равно показывался
     * нерешённый вопрос, то есть проверка смотрела не на то, что
     * объявляла. Найдено мутационным прогоном. */
    PermissionRequest done = bash_req(1);
    done.decided = true;
    done.allowed = true;
    const Panel p = build({done, doom_req(2)});
    ASSERT_TRUE(p.visible);
    ASSERT_EQ(p.ask.id, (uint64_t)2);
    ASSERT_TRUE(p.ask.permission == "doom_loop");
}

TEST(решённый_не_считается_прочим) {
    /* Отдельно от предыдущей проверки: в счёт попадает только то, что
     * ещё можно отклонить. Решённый вопрос отклонять нечем, и «Отклонит
     * и ещё 1 вопрос» врало бы ровно на той картине, где человек видит
     * один вопрос и один решенный призрак. */
    PermissionRequest mid = bash_req(2);
    mid.decided = true;
    mid.allowed = true;
    const Panel p = build({bash_req(1), mid, doom_req(3)});
    ASSERT_EQ(p.ask.id, (uint64_t)1);
    ASSERT_EQ(p.others, (std::size_t)1);
    ASSERT_TRUE(p.ask.cascade_line.find("1") != std::string::npos);
}

/* --- Подписи --- */

TEST(заголовок_называет_вопрос_и_его_ключ) {
    /* Ключ в заголовке полезен при разборе того, что правило запретило,
     * поэтому он назван, а не спрятан. */
    ASSERT_TRUE(headline("deploy") == "Требуется разрешение (deploy)");
}

TEST(пустой_ключ_не_даёт_пустых_скобок) {
    /* «Требуется разрешение ()» выглядит как сломанная строка. Проверка
     * смотрит на ОТСУТСТВИЕ, а не сравнивает с константой: константа
     * была бы зелёной ровно до первой правки заголовка, то есть проверяла
     * бы не формулировку, а её совпадение с собой. */
    ASSERT_TRUE(headline("").find("()") == std::string::npos);
    /* Ключ с пробелом внутри скобок остаётся — пустые скобки это другое. */
    ASSERT_TRUE(headline("external_directory").find("external_directory") !=
                std::string::npos);
}

TEST(подпись_всегда_пуста_без_паттерна) {
    ASSERT_TRUE(suggest_line("").empty());
    ASSERT_TRUE(suggest_line("git status*").find("git status*") !=
                std::string::npos);
}

/* --- Связка с живым PermissionEngine --- */

TEST(панель_показывает_тот_вопрос_который_висит) {
    /* Склейка: реальный вопрос из движка → панель → тот же id для ответа.
     * Без неё панель проверялась бы на рукотворной структуре и не знала
     * бы, что её кормить будут настоящим движком. */
    PermGuard g;
    PermissionEngine& pe = engine().permissions();
    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("bash", {"git push"}, "git push*", "bash → git push");
    });

    ASSERT_TRUE(wait_pending(1));
    const Panel p = build(pe.pending());
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(p.ask.permission == "bash");
    ASSERT_TRUE(p.ask.can_always);
    ASSERT_EQ(p.ask.id, pe.pending().front().id);

    ASSERT_TRUE(pe.reply(p.ask.id, PermissionReply::Once));
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);
}

TEST(всегда_по_живому_вопросу_пишет_правило_а_разово_нет) {
    /* Тождество, ради которого панель и делается: «Всегда» пишет правило,
     * «Разрешить разово» — нет. Если бы связь оборвалась, надпись
     * ««Всегда» разрешит: …» стала бы ложью, и никакой тест на подпись
     * бы этого не показал. */
    PermGuard g;
    PermissionEngine& pe = engine().permissions();
    const std::string key = "wp_test_key";
    pe.add_rule(Rule{key, "*", PermissionAction::Ask, ""});

    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask(key, {"deploy-it"}, "deploy-it*", "deploy");
    });
    ASSERT_TRUE(wait_pending(1));

    Panel p = build(pe.pending());
    ASSERT_TRUE(p.ask.can_always);
    ASSERT_TRUE(pe.reply(p.ask.id, PermissionReply::Always));
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);
    /* Правило записалось: тот же вопрос больше не задаётся. */
    ASSERT_TRUE(pe.evaluate(key, "deploy-it") == PermissionAction::Allow);
}

TEST(кнопки_всегда_нет_у_живого_вопроса_без_паттерна) {
    /* То же, но на живом движке: у doom_loop suggested пуст намеренно, и
     * панель обязана это увидеть из САМОГО запроса, а не из подписи. */
    PermGuard g;
    PermissionEngine& pe = engine().permissions();
    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("doom_loop", {"bash"}, "", "Зацикливание: bash");
    });
    ASSERT_TRUE(wait_pending(1));
    const Panel p = build(pe.pending());
    ASSERT_FALSE(p.ask.can_always);
    /* Ответ «всегда» на такой вопрос всё равно разрешил бы его — и НЕ
     * записал бы правила. Именно поэтому кнопки на экране нет. */
    ASSERT_TRUE(pe.reply(p.ask.id, PermissionReply::Once));
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);
}

/* --- Граница с окном --- */

TEST(окно_берёт_решение_панели_из_core) {
    /* Окно ImGui юнит-тестом не проверяется, поэтому сверяем по исходнику:
     * решение пришло из core, а окно его только рисует. Правка решения
     * обратно в окно расходилась бы с проверками панели молча — они бы
     * продолжали зелёными проверять функцию, которую никто не зовёт. */
    std::ifstream f(fs::path(__FILE__).parent_path().parent_path() /
                        "ui/coder_window.cpp",
                    std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string ui = ss.str();
    ASSERT_FALSE(ui.empty());

    const std::size_t at = ui.find("static void render_permission_dialog()");
    ASSERT_TRUE(at != std::string::npos);
    const std::size_t end = ui.find("\nstatic ", at + 10);
    /* Сравнивается код без комментариев: слова в комментарии —
     * объяснение, а не решение, и проверка не должна падать на нём. */
    const std::string body = strip_comments(
        ui.substr(at, end == std::string::npos ? std::string::npos : end - at));

    /* Решение пришло из core. */
    ASSERT_TRUE(body.find("permission_panel::build(") != std::string::npos);
    /* Ответ уходит в движок по id, а не правится на месте. */
    ASSERT_TRUE(body.find("engine().permission_reply(") != std::string::npos);
    ASSERT_TRUE(body.find("engine().permissions()") == std::string::npos);
    /* Подписи не собираются в окне: окно читает готовые поля, но не
     * склеивает их само. Признак сборки — конкатенация строк и вызов
     * suggest_line/cascade_line, собирающих текст; обращение к полю
     * `.c_str()` при рисовании — это как раз использование решения core,
     * и его запрещать нельзя. */
    ASSERT_TRUE(body.find("suggest_line +=") == std::string::npos);
    ASSERT_TRUE(body.find("cascade_line +=") == std::string::npos);
    ASSERT_TRUE(body.find("\"Требуется разрешение") == std::string::npos);
    ASSERT_TRUE(body.find("\"Разрешить") == std::string::npos);
    ASSERT_TRUE(body.find("\"Всегда") == std::string::npos);
    ASSERT_TRUE(body.find("\"Отклонить") == std::string::npos);
    /* Наличие кнопки «Всегда» — тоже решение core, а не проверка
     * suggested.empty() в окне. */
    ASSERT_TRUE(body.find("suggested.empty()") == std::string::npos);
}