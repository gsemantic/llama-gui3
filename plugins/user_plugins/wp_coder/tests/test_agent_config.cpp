/*
 * test_agent_config.cpp — И8.2: агенты из `.wpcode/agent/*.md`.
 *
 * Разбор frontmatter проверяется на ТЕКСТАХ (без файловой системы) —
 * это и быстрее, и видно, что именно проверяется: парсер не должен
 * молча превращать незнакомое в пустое. Загрузка каталога проверяется на
 * настоящих файлах, потому что её работа начинается там: имя агента
 * берётся из ИМЕНИ ФАЙЛА, а не из содержимого.
 *
 * Отдельным тестом проверяется вызов из Engine::load_settings: разбор и
 * загрузка, существующие сами по себе, ничего не значат, если их никто
 * не зовёт — это класс D2 в чистом виде, и весь конвейер можно было бы
 * откатить к пустому `add()`, оставив все проверки зелёными.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/agent_registry.h"
#include "../core/base_tools.h"
#include "../core/engine.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

/* Диагностика, содержащая подстроку (пустой результат — тест падает). */
bool any_diag_contains(const std::vector<AgentLoadDiag>& diags,
                       const std::string& needle) {
    for (const AgentLoadDiag& d : diags) {
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string all_diags(const std::vector<AgentLoadDiag>& diags) {
    std::string out;
    for (const AgentLoadDiag& d : diags) {
        if (!out.empty()) out += " | ";
        out += d.message;
    }
    return out;
}

fs::path make_tmp_tree(const std::string& tag) {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_agents_" + tag + "_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

fs::path plugin_root() {
    return fs::path(__FILE__).parent_path().parent_path();
}

std::string read_core_file(const std::string& name) {
    std::ifstream f(plugin_root() / "core" / name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/* Тело функции по имени: от открывающей скобки до парной закрывающей. */
std::string function_body(const std::string& src, const std::string& signature) {
    const size_t at = src.find(signature);
    if (at == std::string::npos) return "";
    size_t i = src.find('{', at);
    if (i == std::string::npos) return "";
    size_t depth = 0;
    for (size_t j = i; j < src.size(); ++j) {
        if (src[j] == '{') ++depth;
        else if (src[j] == '}') {
            if (--depth == 0) return src.substr(i + 1, j - i - 1);
        }
    }
    return "";
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

} // namespace

/* ======================================================================
 * 1. Разбор одного файла
 * ====================================================================== */

TEST(agent_config_parses_every_declared_field) {
    const std::string text =
        "---\n"
        "description: Обзор проекта\n"
        "mode: subagent\n"
        "model: qwen3-coder\n"
        "temperature: 0.2\n"
        "top_p: 0.95\n"
        "steps: 8\n"
        "color: green\n"
        "hidden: true\n"
        "tools:\n"
        "  read: true\n"
        "  bash: false\n"
        "permission:\n"
        "  bash: deny\n"
        "  edit:\n"
        "    \"*\": ask\n"
        "    \"*.env\": deny\n"
        "retries: \"2\"\n"
        "---\n"
        "Ищи по проекту и отвечай коротко.\n";

    AgentDef def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown(text, "explore", &def, &diags));
    if (!diags.empty()) std::cerr << "  замечания: " << all_diags(diags) << "\n";

    ASSERT_EQ(def.name, std::string("explore"));
    ASSERT_EQ(def.description, std::string("Обзор проекта"));
    ASSERT_EQ(std::string(agent_mode_name(def.mode)), std::string("subagent"));
    ASSERT_EQ(def.model, std::string("qwen3-coder"));
    ASSERT_TRUE(def.hidden);
    ASSERT_TRUE(def.has_temperature);
    ASSERT_EQ(def.temperature, 0.2);
    ASSERT_TRUE(def.has_top_p);
    ASSERT_EQ(def.top_p, 0.95);
    ASSERT_TRUE(def.has_steps);
    ASSERT_EQ(def.steps, 8);
    ASSERT_EQ(def.color, std::string("green"));
    ASSERT_EQ(def.tools.size(), (size_t)2);
    ASSERT_TRUE(def.tools.at("read"));
    ASSERT_FALSE(def.tools.at("bash"));
    ASSERT_EQ(def.options.at("retries"), std::string("2"));
    ASSERT_EQ(def.prompt, std::string("Ищи по проекту и отвечай коротко."));

    /* Порядок правил объявления — семантика (last match wins), поэтому
     * он проверяется списком, а не «есть ли такие-то». */
    ASSERT_EQ(def.permission.size(), (size_t)3);
    ASSERT_EQ(def.permission[0].key, std::string("bash"));
    ASSERT_EQ(def.permission[0].pattern, std::string("*"));
    ASSERT_EQ(std::string(permission_action_name(def.permission[0].action)),
              std::string("запретить"));
    ASSERT_EQ(def.permission[1].key, std::string("edit"));
    ASSERT_EQ(def.permission[1].pattern, std::string("*"));
    ASSERT_EQ(def.permission[2].pattern, std::string("*.env"));
    ASSERT_EQ(std::string(permission_action_name(def.permission[2].action)),
              std::string("запретить"));
}

TEST(agent_config_body_is_the_prompt_and_prompt_key_overrides_it) {
    AgentDef body_def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\ndescription: Только описание\n---\nПервая строка.\nВторая.\n",
        "body_only", &body_def, &diags));
    ASSERT_EQ(body_def.prompt, std::string("Первая строка.\nВторая."));

    /* Ключ `prompt` перекрывает тело — и оба сразу это запрещено
     * молча решать: два промпта у одного агента означают, что правит
     * один из них, а какой — узнать нельзя. */
    AgentDef both;
    ASSERT_TRUE(parse_agent_markdown(
        "---\ndescription: D\nprompt: Из ключа\n---\nИз тела\n",
        "both", &both, &diags));
    ASSERT_EQ(both.prompt, std::string("Из ключа"));
    ASSERT_TRUE(any_diag_contains(diags, "prompt"));
}

TEST(agent_config_requires_frontmatter) {
    AgentDef def;
    std::vector<AgentLoadDiag> diags;

    /* Без frontmatter агент не регистрируется: принятый молча файл дал
     * бы агента без описания и без промпта — пустую строку в списке. */
    ASSERT_FALSE(parse_agent_markdown("Просто текст без рамки.\n", "x", &def, &diags));
    /* Сообщение различается: «нет рамки» и «рамка не закрыта» — разные
     * ошибки, и проверка на слово «frontmatter» удовлетворялась бы любой
     * из них (то есть не проверяла бы ничего). */
    if (!any_diag_contains(diags, "должен начинаться")) {
        std::cerr << "  для файла без рамки сказано: " << all_diags(diags) << std::endl;
    }
    ASSERT_TRUE(any_diag_contains(diags, "должен начинаться"));
    ASSERT_FALSE(any_diag_contains(diags, "не закрыт"));

    std::vector<AgentLoadDiag> unclosed;
    ASSERT_FALSE(parse_agent_markdown("---\ndescription: D\n", "y", &def, &unclosed));
    ASSERT_TRUE(any_diag_contains(unclosed, "не закрыт"));
}

TEST(agent_config_unknown_mode_is_rejected_entirely) {
    /* Единственный строгий отказ: режим решает, может ли агент править
     * файлы. «Неизвестно → all» превратил бы опечатку в разрешение. */
    AgentDef def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_FALSE(parse_agent_markdown(
        "---\ndescription: D\nmode: primry\n---\nТело\n", "typo", &def, &diags));
    ASSERT_TRUE(any_diag_contains(diags, "primry"));
    ASSERT_TRUE(any_diag_contains(diags, "primary, subagent, all"));

    /* Известные значения и отсутствие ключа — обычные случаи. */
    for (const char* m : {"primary", "subagent", "all"}) {
        AgentDef ok;
        std::vector<AgentLoadDiag> none;
        ASSERT_TRUE(parse_agent_markdown(std::string("---\ndescription: D\nmode: ") +
                                             m + "\n---\nT\n",
                                         "ok", &ok, &none));
        ASSERT_TRUE(none.empty());
    }
    AgentDef no_mode;
    std::vector<AgentLoadDiag> none;
    ASSERT_TRUE(parse_agent_markdown("---\ndescription: D\n---\nT\n", "nm", &no_mode, &none));
    ASSERT_EQ(std::string(agent_mode_name(no_mode.mode)), std::string("all"));
}

TEST(agent_config_reports_bad_numbers_and_keeps_defaults) {
    struct Case { const char* text; };
    const std::vector<std::string> bad_numbers = {
        "0.2abc",   /* std::stod взял бы префикс 0.2 */
        "nan",      /* std::stod разбирает, а провайдер — нет */
        "inf",
        "eight",
        "",
        "\"0.3\"",  /* кавычки снимаются: число допустимо */
    };
    for (const std::string& raw : bad_numbers) {
        if (raw == "\"0.3\"") continue;   /* разбирается в другом тесте */
        AgentDef def;
        std::vector<AgentLoadDiag> diags;
        const bool ok = parse_agent_markdown(
            "---\ndescription: D\ntemperature: " + raw + "\nsteps: 3\n---\nT\n",
            "n", &def, &diags);
        ASSERT_TRUE(ok);   /* агент остаётся рабочим */
        if (def.has_temperature) {
            std::cerr << "  temperature: «" << raw << "» принято как "
                      << def.temperature << std::endl;
        }
        ASSERT_FALSE(def.has_temperature);
        ASSERT_TRUE(any_diag_contains(diags, "temperature"));
    }
    /* Целое требует целого: 3.5 — не «шаги». */
    AgentDef half;
    std::vector<AgentLoadDiag> half_diags;
    ASSERT_TRUE(parse_agent_markdown("---\ndescription: D\nsteps: 3.5\n---\nT\n",
                                     "h", &half, &half_diags));
    ASSERT_FALSE(half.has_steps);
    ASSERT_TRUE(any_diag_contains(half_diags, "steps"));

    /* Ноль — заданное значение, а не «не задано»: без признака
     * temperature: 0 молча стала бы дефолтом провайдера. */
    AgentDef zero;
    std::vector<AgentLoadDiag> zero_diags;
    ASSERT_TRUE(parse_agent_markdown("---\ndescription: D\ntemperature: 0\n---\nT\n",
                                     "z", &zero, &zero_diags));
    ASSERT_TRUE(zero.has_temperature);
    ASSERT_EQ(zero.temperature, 0.0);
    ASSERT_TRUE(zero_diags.empty());

    /* Кавычки — часть значения, а не мусор. */
    AgentDef quoted;
    std::vector<AgentLoadDiag> quoted_diags;
    ASSERT_TRUE(parse_agent_markdown("---\ndescription: D\nmodel: \"my/model:v2\"\n---\nT\n",
                                     "q", &quoted, &quoted_diags));
    ASSERT_EQ(quoted.model, std::string("my/model:v2"));
    ASSERT_TRUE(quoted_diags.empty());
}

TEST(agent_config_reports_bad_flags_and_actions) {
    AgentDef def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\ndescription: D\nhidden: maybe\npermission:\n  bash: sometimes\n---\nT\n",
        "f", &def, &diags));
    ASSERT_FALSE(def.hidden);
    ASSERT_EQ(def.permission.size(), (size_t)0);
    ASSERT_TRUE(any_diag_contains(diags, "hidden"));
    ASSERT_TRUE(any_diag_contains(diags, "sometimes"));
}

TEST(agent_config_tools_accept_map_and_list_forms) {
    AgentDef as_list;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\ndescription: D\ntools: [read, grep]\n---\nT\n", "l", &as_list, &diags));
    ASSERT_EQ(as_list.tools.size(), (size_t)2);
    ASSERT_TRUE(as_list.tools.at("read"));
    ASSERT_TRUE(as_list.tools.at("grep"));

    /* Плохое значение в карте: инструмент пропускается, остальные
     * читаются — иначе одна опечатка обнуляла бы весь список. */
    AgentDef mixed;
    std::vector<AgentLoadDiag> mixed_diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\ndescription: D\ntools:\n  read: true\n  bash: perhaps\n  edit: false\n"
        "---\nT\n", "m", &mixed, &mixed_diags));
    ASSERT_EQ(mixed.tools.size(), (size_t)2);
    ASSERT_TRUE(mixed.tools.at("read"));
    ASSERT_FALSE(mixed.tools.at("edit"));
    ASSERT_TRUE(any_diag_contains(mixed_diags, "perhaps"));
}

TEST(agent_config_unknown_keys_become_options_and_comments_are_ignored) {
    AgentDef def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\n"
        "# строка-комментарий\n"
        "description: D   # хвостовой комментарий — часть значения\n"
        "\n"
        "retries: 2\n"
        "labels: [a, b]\n"
        "nested:\n"
        "  deep: 1\n"
        "deeper:\n"
        "  mid:\n"
        "    low: x\n"
        "---\n"
        "Т\n",
        "o", &def, &diags));
    ASSERT_EQ(def.description, std::string("D   # хвостовой комментарий — часть значения"));
    ASSERT_EQ(def.options.at("retries"), std::string("2"));
    ASSERT_EQ(def.options.at("labels"), std::string("a, b"));
    /* Вложенность РАЗВОРАЧИВАЕТСЯ в имя через точку: у options остаётся
     * один тип (строка), а вложенность при этом не теряется. */
    ASSERT_EQ(def.options.at("nested.deep"), std::string("1"));
    /* Глубже одного уровня не поддержано — и сказано об этом, а не
     * превращено в пустую строку. */
    ASSERT_TRUE(def.options.find("deeper.mid.low") == def.options.end());
    ASSERT_TRUE(any_diag_contains(diags, "deeper.mid"));
    /* Известные ключи в options не попадают — иначе одно и то же
     * значение лежало бы в двух местах. */
    ASSERT_TRUE(def.options.find("description") == def.options.end());
    ASSERT_TRUE(def.options.find("mode") == def.options.end());
}

TEST(agent_config_handles_crlf_and_duplicate_keys) {
    /* Конфиг мог быть написан на другой системе. `mode: subagent\r` не
     * совпало бы ни с чем — агент молча исчез бы из списка. */
    AgentDef crlf;
    std::vector<AgentLoadDiag> crlf_diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\r\ndescription: D\r\nmode: subagent\r\nhidden: yes\r\n---\r\nТ\r\n",
        "c", &crlf, &crlf_diags));
    ASSERT_EQ(std::string(agent_mode_name(crlf.mode)), std::string("subagent"));
    ASSERT_TRUE(crlf.hidden);
    ASSERT_EQ(crlf.prompt, std::string("Т"));
    if (!crlf_diags.empty()) std::cerr << "  замечания: " << all_diags(crlf_diags) << "\n";
    ASSERT_TRUE(crlf_diags.empty());

    /* Повтор ключа: последнее значение (как в YAML) + замечание. */
    AgentDef dup;
    std::vector<AgentLoadDiag> dup_diags;
    ASSERT_TRUE(parse_agent_markdown(
        "---\ndescription: Первое\ndescription: Второе\n---\nТ\n", "d", &dup, &dup_diags));
    ASSERT_EQ(dup.description, std::string("Второе"));
    ASSERT_TRUE(any_diag_contains(dup_diags, "повторяется"));
}

TEST(agent_config_warns_about_missing_description_and_empty_prompt) {
    AgentDef def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown("---\nmode: primary\n---\n", "s", &def, &diags));
    ASSERT_TRUE(any_diag_contains(diags, "description"));
    ASSERT_TRUE(any_diag_contains(diags, "промпт"));
    ASSERT_EQ(def.prompt, std::string(""));
}

/* ======================================================================
 * 2. Загрузка каталога и вызов из движка
 * ====================================================================== */

TEST(agent_registry_loads_directory_and_names_agents_by_file) {
    const fs::path tmp = make_tmp_tree("load");
    const fs::path dir = tmp / ".wpcode" / "agent";
    write_file(dir / "beta.md",
               "---\ndescription: Бета\nmode: subagent\n---\nТело бета\n");
    write_file(dir / "alpha.md",
               "---\ndescription: Альфа\nmode: primary\n---\nТело альфа\n");
    write_file(dir / "gamma.md",
               "---\ndescription: Гамма\nmode: primary\n---\nТело гаммы\n");
    /* Не агент: расширение другое и рамки нет. */
    write_file(dir / "notes.txt", "просто заметка");
    write_file(dir / "README.md", "без frontmatter");

    AgentRegistry reg;
    const std::vector<AgentLoadDiag> diags = reg.load_directory(dir.string());

    /* Имя агента берётся из ИМЕНИ ФАЙЛА, а не из содержимого: файл
     * называет агента, содержимое — его промпт. */
    ASSERT_EQ(reg.size(), (size_t)3);
    const auto alpha = reg.find("alpha");
    ASSERT_TRUE(alpha != nullptr);
    ASSERT_EQ(alpha->description, std::string("Альфа"));
    ASSERT_EQ(alpha->prompt, std::string("Тело альфа"));
    ASSERT_TRUE(alpha->source_path.find("alpha.md") != std::string::npos);

    /* Порядок — алфавитный по ИМЕНИ ФАЙЛА, а не тот, в каком каталог
     * отдаёт проход: от него зависит, кто победит при подмене по имени.
     * Проверяется СПИСКОМ из нескольких имён: список из одного элемента
     * не наблюдал бы порядок вовсе (и такой проверке нельзя поверить). */
    std::string all_names;
    for (const auto& p : reg.all()) {
        if (!all_names.empty()) all_names += ",";
        all_names += p->name;
    }
    if (all_names != "alpha,beta,gamma") {
        std::cerr << "  порядок регистрации: " << all_names << std::endl;
    }
    ASSERT_EQ(all_names, std::string("alpha,beta,gamma"));
    std::string primaries, subagents;
    for (const std::string& n : reg.primary_names()) {
        if (!primaries.empty()) primaries += ",";
        primaries += n;
    }
    for (const std::string& n : reg.subagent_names()) {
        if (!subagents.empty()) subagents += ",";
        subagents += n;
    }
    ASSERT_EQ(primaries, std::string("alpha,gamma"));
    ASSERT_EQ(subagents, std::string("beta"));

    /* README.md без frontmatter — замечание с ПУТЁМ: без него
     * непонятно, о каком файле речь. */
    ASSERT_TRUE(any_diag_contains(diags, "frontmatter"));
    bool names_file = false;
    bool touched_txt = false;
    for (const AgentLoadDiag& d : diags) {
        if (d.path.find("README.md") != std::string::npos) names_file = true;
        /* notes.txt — не агент, и он даже не должен открываться: иначе
         * каталог с документацией начнёт сыпать замечаниями, и читатель
         * перестанет их читать. */
        if (d.path.find("notes.txt") != std::string::npos) touched_txt = true;
    }
    if (!names_file) {
        std::cerr << "  диагностика не назвала файл: " << all_diags(diags) << std::endl;
    }
    if (touched_txt) {
        std::cerr << "  файл не .md дал замечание: " << all_diags(diags) << std::endl;
    }
    ASSERT_TRUE(names_file);
    ASSERT_FALSE(touched_txt);

    fs::remove_all(tmp);
}

TEST(agent_registry_case_collision_resolves_by_sorted_order) {
    /* «Foo.md» и «foo.md» — одно и то же имя после нормализации. Побеждает
     * тот, кто позже в отсортированном списке («foo.md»: 'f' > 'F'),
     * то есть результат не зависит от того, в каком порядке каталог
     * отдал файлы. */
    const fs::path tmp = make_tmp_tree("case");
    const fs::path dir = tmp / "agent";
    write_file(dir / "Foo.md", "---\ndescription: верхний\n---\nТ\n");
    write_file(dir / "foo.md", "---\ndescription: нижний\n---\nТ\n");

    AgentRegistry reg;
    reg.load_directory(dir.string());
    ASSERT_EQ(reg.size(), (size_t)1);
    const auto a = reg.find("foo");
    ASSERT_TRUE(a != nullptr);
    ASSERT_EQ(a->description, std::string("нижний"));

    fs::remove_all(tmp);
}

TEST(agent_registry_keeps_good_agents_when_one_file_is_bad) {
    const fs::path tmp = make_tmp_tree("bad");
    const fs::path dir = tmp / ".wpcode" / "agent";
    write_file(dir / "good.md", "---\ndescription: Хороший\n---\nТ\n");
    write_file(dir / "bad_mode.md", "---\ndescription: D\nmode: prmary\n---\nТ\n");
    write_file(dir / "My Agent.md", "---\ndescription: Пробел в имени\n---\nТ\n");

    AgentRegistry reg;
    const std::vector<AgentLoadDiag> diags = reg.load_directory(dir.string());

    /* Один плохой файл не отменяет остальные — иначе опечатка в одном
     * имени убирала бы всех агентов проекта. */
    ASSERT_EQ(reg.size(), (size_t)1);
    ASSERT_TRUE(reg.find("good") != nullptr);
    ASSERT_TRUE(any_diag_contains(diags, "prmary"));
    /* Имя из имени файла проверяет РЕЕСТР: правило имени должно быть
     * одно, иначе загрузчик и реестр разойдутся на этом файле. */
    ASSERT_TRUE(any_diag_contains(diags, "пробел") || any_diag_contains(diags, "' '"));

    fs::remove_all(tmp);
}

TEST(agent_registry_missing_agent_directory_is_not_an_error) {
    AgentRegistry reg;
    const std::vector<AgentLoadDiag> diags =
        reg.load_directory("/tmp/wp_coder_agents_нет_такого_каталога");
    /* Каталога нет у большинства проектов; предупреждение при каждой
     * инициализации движка было бы шумом. */
    ASSERT_TRUE(diags.empty());
    ASSERT_EQ(reg.size(), (size_t)0);
}

TEST(engine_load_settings_loads_project_agents) {
    /* Проверка дотягивается до места, где зовут: разбор и загрузка
     * сами по себе ничего не значат, если Engine их не зовёт. */
    const fs::path tmp = make_tmp_tree("engine");
    const fs::path dir = tmp / ".wpcode" / "agent";
    write_file(dir / "reviewer.md",
               "---\ndescription: Ревьюер\nmode: subagent\n---\nСмотри код.\n");

    std::map<std::string, std::string> settings;
    settings["wp_coder.project_dir"] = tmp.string();
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) {
        return false;
    };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) {
        return false;
    };
    cb.llm_is_connected = []() { return false; };
    cb.path_data_dir = []() { return std::string(); };
    cb.path_config_dir = []() { return std::string(); };
    cb.settings_get = [&](const std::string& key, const std::string& def) -> std::string {
        auto it = settings.find(key);
        return it != settings.end() ? it->second : def;
    };
    cb.settings_set = [&](const std::string& key, const std::string& value) {
        settings[key] = value;
    };
    cb.chat_event = [](const std::string&) {};

    auto& eng = Engine::instance();
    eng.init(cb);

    const auto reviewer = AgentRegistry::instance().find("reviewer");
    if (reviewer == nullptr) {
        std::cerr << "  Engine::load_settings не загрузил агента из "
                  << dir.string() << std::endl;
    }
    ASSERT_TRUE(reviewer != nullptr);
    ASSERT_EQ(reviewer->description, std::string("Ревьюер"));
    ASSERT_EQ(reviewer->prompt, std::string("Смотри код."));
    ASSERT_EQ(AgentRegistry::instance().subagent_names().size(), (size_t)1);

    /* Восстановление: Engine — синглтон, и подмена project_dir без
     * возврата уронила бы следующий тест, а не этот. */
    AgentRegistry::instance().clear();
    settings.erase("wp_coder.project_dir");
    eng.load_settings();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().project_dir, std::string(""));
    }

    fs::remove_all(tmp);
}

/* ======================================================================
 * 3. Нормализация: `tools` и `permission` → правила (И8.3)
 * ====================================================================== */

TEST(agent_rules_turn_tools_into_permission_rules) {
    register_base_tools();
    AgentDef def;
    def.name = "t";
    def.tools["bash"] = false;
    def.tools["read_file"] = true;      /* имя инструмента, не ключ */
    def.tools["write_file"] = false;    /* ключ этого инструмента — write */

    const Ruleset rules = normalized_agent_rules(def);

    /* Имя ИНСТРУМЕНТА приводится к его ключу, иначе правило «read_file →
     * разрешить» не сработало бы ни на одном чтении: enforcement
     * спрашивает по ключу, а не по имени инструмента. */
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("read", "*"))),
              std::string("разрешить"));
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("bash", "*"))),
              std::string("запретить"));
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("write", "*"))),
              std::string("запретить"));
    /* Правила ровно три: запись «read_file» не осталась вторым,
     * параллельным правилом по несуществующему ключу. */
    ASSERT_EQ(rules.size(), (size_t)3);
    for (const Rule& r : rules.rules()) {
        if (r.permission == "read_file") {
            std::cerr << "  осталось правило по имени инструмента: " << r.permission
                      << std::endl;
        }
        ASSERT_TRUE(r.permission != "read_file");
    }
}

TEST(agent_rules_permission_overrides_tools) {
    register_base_tools();
    AgentDef def;
    def.name = "p";
    def.tools["bash"] = false;
    PermissionEntry ask_all;
    ask_all.key = "bash";
    ask_all.action = PermissionAction::Ask;
    PermissionEntry allow_git;
    allow_git.key = "bash";
    allow_git.pattern = "git status*";
    allow_git.action = PermissionAction::Allow;
    def.permission = {ask_all, allow_git};

    const Ruleset rules = normalized_agent_rules(def);

    /* `permission` идёт ПОСЛЕ `tools` и перекрывает его: у него есть
     * паттерн, то есть он точнее. Обратный порядок отдал бы победу
     * `tools`, и `bash: ask` не смог бы перекрыть `bash: false` —
     * более сильное правило просто не работало бы. */
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("bash", "git status"))),
              std::string("разрешить"));
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("bash", "rm -rf /"))),
              std::string("спросить"));
}

TEST(agent_rules_fold_edit_aliases_into_the_write_key) {
    register_base_tools();
    AgentDef def;
    def.name = "plan";
    def.tools["edit"] = false;          /* словарь порта */
    PermissionEntry patch_deny;
    patch_deny.key = "patch";
    patch_deny.action = PermissionAction::Deny;
    def.permission.push_back(patch_deny);

    std::vector<AgentLoadDiag> diags;
    const Ruleset rules = normalized_agent_rules(def, &diags);

    /* `edit`, `patch` и `multiedit` — имена из словаря opencode, а
     * наш ключ группы называется `write`. Приводим чужие имена к
     * нашему ключу, а не свой — к чужому: переименование ключа
     * обнулило бы уже сохранённые правила пользователя. */
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("write", "*"))),
              std::string("запретить"));
    ASSERT_TRUE(rules.denies_all("write"));
    /* Приведение видно в замечаниях: иначе человек смотрит на `edit`
     * в своём конфиге, а агент отвечает по `write`. */
    ASSERT_TRUE(any_diag_contains(diags, "edit"));
    ASSERT_TRUE(any_diag_contains(diags, "write"));

    /* Скрытие инструментов этой группы (И2.8) работает по тому же
     * ключу — то есть агент с `tools: {edit: false}` действительно
     * не получает пишущие инструменты, а не только помечает их. */
    const std::vector<ToolDef> defs = ToolsRegistry::instance().defs();
    size_t hidden = 0;
    for (const ToolDef& d : defs) {
        if (rules.denies_all(permission_key_of(d))) ++hidden;
    }
    if (hidden < 5) {
        std::cerr << "  правилом «edit: false» скрыто инструментов: " << hidden
                  << std::endl;
    }
    ASSERT_TRUE(hidden >= 5);
}

TEST(agent_rules_keep_declaration_order_and_patterns) {
    register_base_tools();
    AgentDef def;
    def.name = "o";
    PermissionEntry deny_env;
    deny_env.key = "edit";
    deny_env.pattern = "*.env";
    deny_env.action = PermissionAction::Deny;
    PermissionEntry allow_env;
    allow_env.key = "edit";
    allow_env.pattern = "*.env";
    allow_env.action = PermissionAction::Allow;
    def.permission = {deny_env, allow_env};

    const Ruleset rules = normalized_agent_rules(def);

    /* Порядок правил = семантика (last match wins). Проверяется
     * СПИСКОМ и по решению: правило с паттерном не должно
     * превратиться в правило по «*» — иначе «*.env → запретить»
     * запрещало бы всё редактирование. */
    ASSERT_EQ(rules.size(), (size_t)2);
    ASSERT_EQ(rules.rules()[0].pattern, std::string("*.env"));
    ASSERT_EQ(rules.rules()[1].pattern, std::string("*.env"));
    ASSERT_EQ(std::string(permission_action_name(
                  rules.evaluate("write", "/srv/.env"))),
              std::string("разрешить"));
    ASSERT_EQ(std::string(permission_action_name(
                  rules.evaluate("write", "/srv/main.cpp"))),
              std::string("спросить"));
}

TEST(agent_rules_unknown_name_is_kept_and_reported) {
    register_base_tools();
    AgentDef def;
    def.name = "u";
    def.tools["frobnicate"] = false;
    def.permission.push_back({"teapot", "*", PermissionAction::Deny});

    std::vector<AgentLoadDiag> diags;
    const Ruleset rules = normalized_agent_rules(def, &diags);

    /* Правило по несуществующему ключу безвредно и заработает, если
     * инструмент появится. Молча выбросить его — значило бы узнать об
     * опечатке по тому, что ограничение не сработало. */
    ASSERT_EQ(rules.size(), (size_t)2);
    ASSERT_TRUE(rules.denies_all("frobnicate"));
    ASSERT_TRUE(rules.denies_all("teapot"));
    ASSERT_TRUE(any_diag_contains(diags, "frobnicate"));
    ASSERT_TRUE(any_diag_contains(diags, "teapot"));
    ASSERT_TRUE(any_diag_contains(diags, "неизвестный"));

    /* Без замечаний нормализация обязана работать: 8.4 зовёт её на
     * каждом агенте и не обязан заводить список ради пустоты. */
    const Ruleset quiet = normalized_agent_rules(def);
    ASSERT_EQ(quiet.size(), (size_t)2);
}

TEST(agent_rules_empty_agent_asks_rather_than_allows) {
    AgentDef def;   /* ничего не объявлено */
    const Ruleset rules = normalized_agent_rules(def);
    ASSERT_TRUE(rules.empty());
    /* Отсутствие правил — это НЕ «разрешить» (core/permission.h):
     * не описанное поведение спрашивает. Иначе агент без конфига
     * получил бы все права молча. */
    ASSERT_EQ(std::string(permission_action_name(rules.evaluate("bash", "*"))),
              std::string("спросить"));
    ASSERT_FALSE(rules.denies_all("bash"));
}

TEST(agent_rules_key_mapping_follows_the_tool_registry) {
    register_base_tools();
    /* Синтетический инструмент с собственным ключом: если бы отображение
     * имени в ключ было зашитой таблицей, такой инструмент в неё бы не
     * попал — и правило по нему заработало бы «случайно». */
    ToolDef synth;
    synth.name = "test_synth_writer";
    synth.description = "синтетический инструмент для проверки отображения";
    synth.flags = TF_READ_ONLY;
    synth.permission_key = "test_synth_key";
    synth.parameters = SchemaBuilder().build();
    synth.handler = [](const json::JsonValue&, ToolContext&) { return ToolOutput(); };
    ToolsRegistry::instance().register_def(synth);

    bool known = false;
    ASSERT_EQ(canonical_permission_key("test_synth_writer", &known),
              std::string("test_synth_key"));
    ASSERT_TRUE(known);

    /* Имя, которое УЖЕ является ключом, остаётся собой: «bash» не
     * должен превращаться в «bash: один инструмент». */
    known = false;
    ASSERT_EQ(canonical_permission_key("bash", &known), std::string("bash"));
    ASSERT_TRUE(known);

    /* Ключ, которого нет среди имён инструментов, но который принадлежит
     * инструментам (у todowrite/todoread ключ «todo», а инструмента
     * «todo» нет): правило «todo: запретить» обязано быть правилом по
     * этому ключу, иначе оно не сработает ни на одном из них. */
    known = false;
    ASSERT_EQ(canonical_permission_key("todo", &known), std::string("todo"));
    ASSERT_TRUE(known);

    /* Совершенно неизвестное имя — как есть, но с признаком. */
    known = true;
    ASSERT_EQ(canonical_permission_key("no_such_thing", &known),
              std::string("no_such_thing"));
    ASSERT_FALSE(known);
}

/* ======================================================================
 * 4. Рантайм-структура агента (И8.4)
 * ====================================================================== */

namespace {

/* База сессии: правила, которые агент перекрывает своим определением. */
Ruleset base_session_rules() {
    Ruleset base;
    base.add("read", "*", PermissionAction::Allow);
    base.add("bash", "*", PermissionAction::Ask);
    base.add("write", "*", PermissionAction::Ask);
    return base;
}

} // namespace

TEST(agent_info_carries_every_parsed_field) {
    register_base_tools();
    const std::string text =
        "---\ndescription: Планировщик\nmode: primary\nmodel: qwen3\n"
        "temperature: 0.3\ntop_p: 0.8\nsteps: 6\ncolor: blue\n"
        "tools:\n  bash: false\noptions:\n  retries: \"2\"\n---\nСначала план.\n";
    AgentDef def;
    std::vector<AgentLoadDiag> diags;
    ASSERT_TRUE(parse_agent_markdown(text, "wp_plan", &def, &diags));

    const auto info = agent::Info::from_def(def, base_session_rules());
    ASSERT_TRUE(info != nullptr);
    ASSERT_EQ(info->name(), std::string("wp_plan"));
    ASSERT_EQ(info->description(), std::string("Планировщик"));
    ASSERT_EQ(std::string(agent_mode_name(info->mode())), std::string("primary"));
    ASSERT_EQ(info->prompt(), std::string("Сначала план."));
    ASSERT_EQ(info->model(), std::string("qwen3"));
    ASSERT_TRUE(info->has_temperature());
    ASSERT_EQ(info->temperature(), 0.3);
    ASSERT_TRUE(info->has_top_p());
    ASSERT_EQ(info->top_p(), 0.8);
    ASSERT_EQ(info->steps(), 6);
    ASSERT_EQ(info->color(), std::string("blue"));
    ASSERT_EQ(info->options().at("retries"), std::string("2"));
    ASSERT_FALSE(info->hidden());
}

TEST(agent_info_rules_override_the_base_and_freeze_it) {
    register_base_tools();
    AgentDef def;
    def.name = "plan";
    def.tools["write"] = false;          /* агент не пишет */
    def.tools["read_file"] = true;       /* своё правило поверх базы */

    const auto info = agent::Info::from_def(def, base_session_rules());

    /* Агент перекрывает базу, а не наоборот: правила агента идут
     * ПОСЛЕ базовых, иначе `write: deny` в конфиге агента никогда бы не
     * сработало — база спросила бы, и агент редактировал бы. */
    ASSERT_EQ(std::string(permission_action_name(info->evaluate("write", "*"))),
              std::string("запретить"));
    /* Правила сессии остаются там, где агент ничего не сказал. */
    ASSERT_EQ(std::string(permission_action_name(info->evaluate("bash", "*"))),
              std::string("спросить"));
    ASSERT_EQ(std::string(permission_action_name(info->evaluate("read", "*"))),
              std::string("разрешить"));

    /* Правила ЗАМОРОЖЕНЫ: наружу отдаётся const, и новых правил у
     * агента не появляется — единственное, что растёт, это `approved`. */
    const size_t before = info->rules().size();
    info->approve("write", "/tmp/*");
    ASSERT_EQ(info->rules().size(), before);
    ASSERT_TRUE(before > 0);
}

TEST(agent_info_approved_wins_over_frozen_rules) {
    register_base_tools();
    AgentDef def;
    def.name = "plan";
    def.tools["write"] = false;

    const auto info = agent::Info::from_def(def, base_session_rules());
    ASSERT_EQ(std::string(permission_action_name(info->evaluate("write", "*"))),
              std::string("запретить"));

    /* Ответ «всегда» новее любого правила конфига — иначе нажатая
     * кнопка ничего не делала бы, и это выглядело бы как поломка. */
    info->approve("write", "*");
    ASSERT_EQ(std::string(permission_action_name(info->evaluate("write", "*"))),
              std::string("разрешить"));
    /* Точечное «всегда» не разрешает всё остальное. */
    const auto narrow = agent::Info::from_def(def, base_session_rules());
    narrow->approve("write", "/tmp/scratch/*");
    ASSERT_EQ(std::string(permission_action_name(
                  narrow->evaluate("write", "/tmp/scratch/a.txt"))),
              std::string("разрешить"));
    ASSERT_EQ(std::string(permission_action_name(
                  narrow->evaluate("write", "/srv/app/main.cpp"))),
              std::string("запретить"));
}

TEST(agent_info_denied_keys_are_precomputed_and_approval_unhides) {
    register_base_tools();
    AgentDef def;
    def.name = "explore";
    def.permission.push_back({"*", "*", PermissionAction::Deny});
    PermissionEntry allow_tools;
    allow_tools.key = "read";
    allow_tools.action = PermissionAction::Allow;
    def.permission.push_back(allow_tools);

    const auto info = agent::Info::from_def(def, base_session_rules());

    /* Список запрещённых целиком ключей посчитан при сборке — по нему
     * И8.5/И8.13 решают, что показывать модели. «* → deny» запрещает
     * всё, кроме явно разрешённого, и именно это в нём и записано. */
    /* «* → deny» накрывает и те ключи, которых агент не упоминал: запрет
     * целиком считается из тех же замороженных правил, а не из
     * собственного списка, который разошёлся бы с ними. */
    ASSERT_TRUE(info->denies_whole_key("bash"));
    ASSERT_TRUE(info->denies_whole_key("write"));
    ASSERT_TRUE(info->denies_whole_key("git"));
    ASSERT_FALSE(info->denies_whole_key("read"));
    /* «* → deny» накрывает и ключи, которых агент не называл: иначе
     * агент «всё запретил, кроме списка» умел бы писать за пределы
     * проекта — то есть ровно то, чего такой агент не должен уметь.
     * Именно так и устроен wp_explore в 8.6. */
    ASSERT_TRUE(info->denies_whole_key("external_directory"));

    /* Ответ «всегда» возвращает инструмент в обзор: иначе разрешение
     * выглядело бы бездействующим — инструмент-то не видно модели. */
    info->approve("bash", "*");
    ASSERT_FALSE(info->denies_whole_key("bash"));
    ASSERT_EQ(std::string(permission_action_name(info->evaluate("bash", "*"))),
              std::string("разрешить"));
}

TEST(agent_info_rejects_nothing_silently_but_reports_negative_steps) {
    register_base_tools();
    AgentDef def;
    def.name = "odd";
    def.has_steps = true;
    def.steps = -3;

    std::vector<AgentLoadDiag> diags;
    const auto info = agent::Info::from_def(def, base_session_rules(), &diags);

    /* Отрицательные шаги — опечатка, а не «агент не делает ничего»:
     * 0 значит «взяты шаги сессии», и об этом сказано. */
    ASSERT_EQ(info->steps(), 0);
    ASSERT_TRUE(any_diag_contains(diags, "steps"));
}

TEST(agent_info_is_shared_and_survives_concurrent_approval) {
    register_base_tools();
    AgentDef def;
    def.name = "shared";
    def.tools["bash"] = false;

    const auto info = agent::Info::from_def(def, base_session_rules());

    /* Рантайм-агента читают worker-поток (решение по каждому вызову) и
     * UI-поток (панель правил); пополняет `approved` UI. Если бы правка
     * шла в общую Ruleset, читатель увидел бы список в момент изменения —
     * то есть ровно тот класс расхождения, который И8.4 и запрещает. */
    std::atomic<bool> stop{false};
    std::atomic<size_t> approved{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                /* Читатель задерживается внутри Ruleset подольше одного
                 * совпадения: вектор правил перевыделяется на каждом
                 * append, и без лока читатель ходит по освобождённой
                 * памяти. Чем длиннее обход, тем уже окно гонки. */
                for (int k = 0; k < 200; ++k) {
                    info->evaluate("bash", "git status " + std::to_string(k));
                    info->evaluate("write", "/srv/a.cpp");
                    info->evaluate("read", "*");
                }
                info->rules().size();
            }
        });
    }
    const int kApproveCount = 20000;
    for (int i = 0; i < kApproveCount; ++i) {
        info->approve("bash", "cmd" + std::to_string(i));
        approved.fetch_add(1);
    }
    stop.store(true);
    for (auto& t : readers) t.join();
    ASSERT_EQ(approved.load(), (size_t)kApproveCount);
    /* Все ответы на месте, а замороженные правила не поехали. */
    ASSERT_EQ(info->rules().size(), (size_t)4);
    ASSERT_EQ(std::string(permission_action_name(
                  info->evaluate("bash", "cmd" + std::to_string(kApproveCount - 1)))),
              std::string("разрешить"));
}

TEST(agent_info_growing_ruleset_is_locked) {
    /* Единственная растущая часть — `approved`: её пишет UI-поток и
     * читает worker-поток на каждом вызове инструмента. Проверка выше
     * ловит гонку только УЖЕ случившуюся (обрыв в прогоне), то есть
     * вероятностно: два прогона подряд — два разных ответа. Поэтому
     * здесь МЕХАНИЧЕСКАЯ проверка самого инварианта: обе точки входа
     * обязаны брать один и тот же мьютекс. Тот же приём, что у
     * проверок структуры (core_does_not_depend_on_the_host_abi). */
    const std::string src = read_core_file("agent_registry.cpp");
    ASSERT_TRUE(!src.empty());

    const std::string approve = function_body(src, "void Info::approve(");
    const std::string evaluate = function_body(src, "PermissionAction Info::evaluate(");
    if (approve.empty() || evaluate.empty()) {
        std::cerr << "  не найдены тела Info::approve/Info::evaluate —"
                  << " проверка молча ничего бы не сделала" << std::endl;
    }
    ASSERT_TRUE(!approve.empty());
    ASSERT_TRUE(!evaluate.empty());

    if (approve.find("approved_mtx_") == std::string::npos) {
        std::cerr << "  Info::approve пишет растущий Ruleset без мьютекса:"
                  << " UI-поток и worker-поток разъедутся" << std::endl;
    }
    if (evaluate.find("approved_mtx_") == std::string::npos) {
        std::cerr << "  Info::evaluate читает растущий Ruleset без мьютекса:"
                  << " читатель ходит по вектору во время append" << std::endl;
    }
    ASSERT_TRUE(approve.find("approved_mtx_") != std::string::npos);
    ASSERT_TRUE(evaluate.find("approved_mtx_") != std::string::npos);
}
