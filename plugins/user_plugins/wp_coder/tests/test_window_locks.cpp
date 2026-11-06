/*
 * test_window_locks.cpp — в окне не держится `st.mtx` под ImGui (И11.12).
 *
 * ---------------------------------------------------------------------------
 * ПОЧЕМУ ПРОВЕРКА ЧИТАЕТ ИСХОДНИК, А НЕ ЗОВЁТ КОД
 * ---------------------------------------------------------------------------
 *
 * Окно — единственный файл плагина, который рисует под ImGui, и единственный,
 * который юнит-тестом не запускается: тестового харнесса для ImGui нет, и
 * заводить его ради шести мест означало бы писать мини-рендерер. Поэтому
 * инвариант проверяется по исходнику окна — тем же приёмом, что
 * `окно_берёт_решение_панели_из_core` (test_permission_panel.cpp) и
 * `окно_берёт_значения_из_core` (test_timeline.cpp, test_token_panel.cpp).
 *
 * Проверяются ДВА разных запрета, и они не взаимозаменяемы:
 *
 *  1. ПОД ЛОКОМ НЕТ IMGUI. Даже без единого рекурсивного вызова это риск:
 *     мьютекс общий с worker-потоком агента (D1), и отрисовка под ним
 *     растягивает чужую блокировку на время, которое ImGui контролирует не
 *     он. Это ровно то, что И11.10 уже убрал из одной кнопки и что
 *     И11.12 убирает у всего окна.
 *
 *  2. ПОД ЛОКОМ НЕТ МЕТОДА `Engine`, КОТОРЫЙ САМ БЕРЁТ `state_.mtx`. Это уже
 *     не риск, а ДЕДЛОК: мьютекс нерекурсивный, и второй захват внутри
 *     первого не вернётся. Так было в блоке «Состояние + профиль» окна
 *     Сессии: `engine().session_profile()` берёт лок сам, а блок держал его
 *     и звал метод из-под лока.
 *
 * ---------------------------------------------------------------------------
 * ПОЧЕМУ ПЕРЕЧЕНЬ МЕТОДОВ НЕ ВЫПИСАН, А ВЫВОДИТСЯ ИЗ engine.cpp
 * ---------------------------------------------------------------------------
 *
 * Перечень «методы Engine, берущие state_.mtx» вычисляется разбором
 * `core/engine.cpp` при запуске проверки. Выписанный вручную список устарел
 * бы молча: новый метод, берущий лок, в него не попал бы — и проверка
 * продолжала бы считать, что окно чисто, а окно зависло бы. Здесь новый
 * метод попадает в запрет САМ, без правки проверки.
 *
 * ---------------------------------------------------------------------------
 * ПОЧЕМУ КОД ЧИТАЕТСЯ БЕЗ КОММЕНТАРИЕВ
 * ---------------------------------------------------------------------------
 *
 * Комментарий в этом файле объясняет, почему лока там нет, — и словами
 * «ImGui» он полон. Проверка ищет ЗАПРЕЩЁННОЕ В КОДЕ, поэтому и код должен
 * быть без комментариев: иначе пришлось бы вычищать из объяснений нужные
 * слова вместо исправления кода (тот же довод, что в `strip_comments`
 * test_permission_panel.cpp).
 */

#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/* Куда смотрим. Путь от __FILE__ — то же, что у соседних проверок границы
 * с окном: тесты собираются в другом каталоге, чем исходники. */
fs::path plugin_root() { return fs::path(__FILE__).parent_path().parent_path(); }

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Вырезать комментарии. Комментарий — объяснение, а не решение, и проверка
 * не должна падать на нём (см. шапку файла). */
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

std::vector<std::string> lines_of(const std::string& src) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

int indent_of(const std::string& l) {
    int n = 0;
    while (n < static_cast<int>(l.size()) && l[n] == ' ') ++n;
    return n;
}

/* Границы блока, открытого `lock_guard<std::mutex> lk(st.mtx);`.
 *
 * Конец — первая непустая строка с отступом МЕНЬШЕ отступа самой строки
 * лока: стиль файла Allman, тело блока всегда глубже, а закрывающая скобка
 * — на уровень выше. Отступ меряется по пробелам, и строка-заглушка внутри
 * блока (отступ 0, например) его не завершит. */
struct Block {
    int first = 0;  /* индекс строки с lock_guard */
    int last = 0;   /* индекс последней строки тела */
};

std::vector<Block> lock_blocks(const std::vector<std::string>& lines) {
    std::vector<Block> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("lock_guard<std::mutex> lk(st.mtx)") == std::string::npos)
            continue;
        Block b;
        b.first = static_cast<int>(i);
        b.last = static_cast<int>(i);
        const int ind = indent_of(lines[i]);
        for (std::size_t j = i + 1; j < lines.size(); ++j) {
            if (lines[j].find_first_not_of(' ') == std::string::npos) continue;
            if (indent_of(lines[j]) < ind) break;
            b.last = static_cast<int>(j);
        }
        out.push_back(b);
    }
    return out;
}

/* Методы Engine, берущие state_.mtx. Выводятся из core/engine.cpp при
 * запуске (шапка файла): перечень, выписанный вручную, устарел бы молча. */
std::set<std::string> engine_methods_taking_lock() {
    std::set<std::string> out;
    const std::vector<std::string> lines =
        lines_of(strip_comments(read_file(plugin_root() / "core/engine.cpp")));

    std::size_t i = 0;
    while (i < lines.size()) {
        /* Сигнатура метода: `Engine::имя(`. */
        const std::size_t at = lines[i].find("Engine::");
        if (at == std::string::npos) { ++i; continue; }
        const std::size_t open = lines[i].find('(', at);
        if (open == std::string::npos) { ++i; continue; }
        std::string name;
        for (std::size_t k = at + 7; k < open; ++k) {
            const char c = lines[i][k];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') name += c;
        }
        if (name.empty()) { ++i; continue; }

        /* Тело — до строки с `}` в нулевом отступе (стиль Allman). */
        bool takes = false;
        std::size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            if (lines[j].find("state_.mtx") != std::string::npos &&
                lines[j].find("lock_guard") != std::string::npos) {
                takes = true;
            }
            if (!lines[j].empty() && lines[j][0] == '}') break;
        }
        if (takes) out.insert(name);
        i = j + 1;
    }
    return out;
}

}  // namespace

/* --- 1. Под локом нет ImGui --- */

TEST(ни_одного_вызова_imgui_под_локом_состояния) {
    const std::vector<std::string> lines =
        lines_of(strip_comments(read_file(plugin_root() / "ui/coder_window.cpp")));
    const std::vector<Block> blocks = lock_blocks(lines);
    /* Пустой перечень — не «чисто», а «проверка ничего не нашла»: такая
     * проверка зелена на любом файле (правило 13 SESSION_START). Сама
     * граница проверяется отдельной строкой ниже. */
    ASSERT_FALSE(blocks.empty());

    for (const Block& b : blocks) {
        for (int i = b.first; i <= b.last; ++i) {
            if (lines[static_cast<std::size_t>(i)].find("ImGui::") ==
                std::string::npos)
                continue;
            std::cerr << "  ImGui под st.mtx, строка " << (i + 1) << ": "
                      << lines[static_cast<std::size_t>(i)] << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

/* --- 2. Под локом нет метода Engine, берущего тот же лок --- */

TEST(ни_одного_метода_engine_под_локом_состояния) {
    /* Это ДЕДЛОК, а не риск: `state_.mtx` нерекурсивный, поэтому второй
     * захват из-под первого не вернётся. Именно так был устроен блок
     * «Состояние + профиль»: `engine().session_profile()` берёт лок сам,
     * а блок держал его и звал метод из-под лока. */
    const std::vector<std::string> lines =
        lines_of(strip_comments(read_file(plugin_root() / "ui/coder_window.cpp")));
    const std::set<std::string> takes = engine_methods_taking_lock();
    /* Список обязан быть непустым: пустой означал бы, что разбор
     * `core/engine.cpp` сломался и запрет не проверяет ничего. */
    ASSERT_FALSE(takes.empty());

    const std::vector<Block> blocks = lock_blocks(lines);
    ASSERT_FALSE(blocks.empty());

    for (const Block& b : blocks) {
        for (int i = b.first; i <= b.last; ++i) {
            const std::string& l = lines[static_cast<std::size_t>(i)];
            const std::size_t at = l.find("engine().");
            if (at == std::string::npos) continue;
            std::string name;
            for (std::size_t k = at + 9; k < l.size(); ++k) {
                const char c = l[k];
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                    name += c;
                else
                    break;
            }
            if (name.empty()) continue;
            if (takes.count(name) == 0) continue;
            std::cerr << "  engine()." << name << " под st.mtx, строка "
                      << (i + 1) << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

/* --- 3. Проверка не зелена вхолостую --- */

TEST(проверка_lock_blocks_видит_блоки_а_не_пустоту) {
    /* Обе проверки выше ищут блоки по строке `lock_guard…`. Если разбор
     * перестанет их находить — правка отступов, смена стиля, перенос
     * блока, — обе станут зелёными на ЛЮБОМ коде, и запрет перестанет
     * проверять что бы то ни было. Здесь это запрещено явно: набор должен
     * быть непустым и содержать ожидаемое число блоков. */
    const std::vector<std::string> lines =
        lines_of(strip_comments(read_file(plugin_root() / "ui/coder_window.cpp")));
    const std::vector<Block> blocks = lock_blocks(lines);
    ASSERT_TRUE(blocks.size() >= 10);
}

TEST(разбор_методов_видит_известный_берущий_лок) {
    /* Перечень запрещённых методов вычисляется разбором `core/engine.cpp`.
     * Проверка знает два метода, которые лок берут наверняка: если хоть один
     * из них выпал из перечня, разбор сломался и запрет выше пуст. */
    const std::set<std::string> takes = engine_methods_taking_lock();
    ASSERT_TRUE(takes.count("session_profile") == 1);   /* дедлок, был в окне */
    ASSERT_TRUE(takes.count("request_abort") == 1);     /* вызывается свободно */
}

TEST(разбор_методов_не_берёт_методы_без_лока) {
    /* Обратная сторона: в запрет не должны попасть методы, которые лок НЕ
     * берут, — иначе проверка запрещала бы безопасные вызовы и её пришлось
     * бы «чинить» ослаблением. `save_settings` лока не берёт (пишет через
     * колбэки), и вызывать его под локом нельзя только из-за ImGui —
     * запрет под локом при этом проверка №1 и делает. */
    const std::set<std::string> takes = engine_methods_taking_lock();
    ASSERT_TRUE(takes.count("save_settings") == 0);
    ASSERT_TRUE(takes.count("permission_pending") == 0);
}

/* --- 4. Проверка ловит именно то, о чём говорит --- */

/*
 * Положительный контроль самой проверки: если бы запрет не работал, его
 * нечем было бы узнать. Поэтому ниже — не «ещё тест», а демонстрация того,
 * что разбор блоков видит ImGui под локом, когда он там ЕСТЬ. Материал
 * собирается в памяти и в файле окна ничего не меняет.
 */
TEST(разбор_видит_imgui_под_локом_когда_он_там_есть) {
    const char* with_imgui_under_lock =
        "void f() {\n"
        "    auto& st = engine_state();\n"
        "    {\n"
        "        std::lock_guard<std::mutex> lk(st.mtx);\n"
        "        ImGui::Text(\"%s\", st.project_dir.c_str());\n"
        "    }\n"
        "}\n";
    const std::vector<std::string> lines =
        lines_of(strip_comments(with_imgui_under_lock));
    const std::vector<Block> blocks = lock_blocks(lines);
    ASSERT_EQ(blocks.size(), size_t(1));

    bool saw_imgui = false;
    for (int i = blocks[0].first; i <= blocks[0].last; ++i) {
        if (lines[static_cast<std::size_t>(i)].find("ImGui::") != std::string::npos)
            saw_imgui = true;
    }
    /* Если бы разбор не видел ImGui внутри блока, запрет выше был бы
     * зелёным всегда — и это вышло бы наружу только как «всё хорошо». */
    ASSERT_TRUE(saw_imgui);
}

TEST(разбор_видит_движок_под_локом_когда_он_там_есть) {
    const char* with_engine_under_lock =
        "void f() {\n"
        "    auto& st = engine_state();\n"
        "    {\n"
        "        std::lock_guard<std::mutex> lk(st.mtx);\n"
        "        coder::harness::Profile p;\n"
        "        std::string why;\n"
        "        engine().session_profile(&p, &why);\n"
        "    }\n"
        "}\n";
    const std::vector<std::string> lines =
        lines_of(strip_comments(with_engine_under_lock));
    const std::set<std::string> takes = engine_methods_taking_lock();
    const std::vector<Block> blocks = lock_blocks(lines);
    ASSERT_EQ(blocks.size(), size_t(1));

    bool saw_engine = false;
    for (int i = blocks[0].first; i <= blocks[0].last; ++i) {
        const std::string& l = lines[static_cast<std::size_t>(i)];
        const std::size_t at = l.find("engine().");
        if (at == std::string::npos) continue;
        std::string name;
        for (std::size_t k = at + 9; k < l.size(); ++k) {
            const char c = l[k];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                name += c;
            else
                break;
        }
        if (name == "session_profile") saw_engine = true;
    }
    ASSERT_TRUE(saw_engine);
    ASSERT_TRUE(takes.count("session_profile") == 1);
}