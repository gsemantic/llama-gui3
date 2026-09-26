/*
 * test_session_store.cpp — И5.6: сессия на диске.
 *
 * Формат файла меняется целиком, поэтому проверяется не «файл читается»,
 * а то, что он не теряет и не выдумывает:
 *
 *  1. Round-trip без потерь для всех девяти видов частей и всех четырёх
 *     состояний вызова. Потеря здесь не «ошибка чтения»: файл читается,
 *     история выглядит правдоподобно, и пропавший кусок обнаруживается
 *     через несколько шагов, когда модель ведёт себя не так.
 *  2. Файл из БОЛЕЕ НОВОЙ версии не читается частично. Иначе старая
 *     версия выбросила бы незнакомое, записала поверх свой файл — и
 *     потеряла историю безвозвратно.
 *  3. Незнакомое внутри знакомого файла (новый вид части, новое состояние
 *     вызова) не обнуляет сессию и не проходит молча: приходит
 *     предупреждением.
 *  4. Запись атомарна: после неё не остаётся .tmp, а неудачная запись не
 *     уничтожает предыдущую.
 *  5. Загрузка поднимает счётчик идентификаторов (И5.5), иначе первый
 *     новый ход получил бы занятый id.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "../core/id_prefix.h"
#include "../core/json_utils.h"
#include "../core/session_store.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path tmp_dir() {
    static int counter = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tmp/wp_coder_store_%d_%d", ++counter,
                  static_cast<int>(::getpid()));
    const fs::path p(buf);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

json::JsonValue read_args() {
    json::JsonValue a = json::JsonValue::object();
    a.set("path", "core/engine.cpp");
    a.set("offset", 42);
    return a;
}

/* Сессия со всеми видами частей и всеми состояниями вызова. */
SessionFile full_session() {
    SessionFile s;
    s.session_id = "ses_000000000001";

    Message task;
    task.id = "msg_000000000010";
    task.role = kRoleUser;
    task.parts.push_back(MessagePart::text("почини цикл"));
    s.messages.push_back(task);

    Message turn;
    turn.id = "msg_000000000011";
    turn.role = kRoleAssistant;
    turn.parent_id = task.id;
    turn.parts.push_back(MessagePart::reasoning("сначала посмотрю цикл"));
    turn.parts.push_back(MessagePart::step_start("step_1"));
    turn.parts.push_back(MessagePart::tool("call_0", "read_file", read_args(),
                                            "{\"tool\":\"read_file\"}"));
    ToolOutput done;
    done.title = "read core/engine.cpp";
    done.output = "прочитал 404 строки";
    done.metadata = json::JsonValue::object();
    done.metadata.set("lines", 404);
    done.truncated = true;
    turn.parts.back().set_result(done);
    turn.parts.push_back(MessagePart::text("файл прочитан"));
    s.messages.push_back(turn);

    Message running;
    running.id = "msg_000000000012";
    running.role = kRoleAssistant;
    running.parts.push_back(MessagePart::tool("call_1", "bash", read_args(),
                                               "{\"tool\":\"bash\"}")
                                .set_running());
    s.messages.push_back(running);

    Message refused;
    refused.id = "msg_000000000013";
    refused.role = kRoleAssistant;
    refused.parts.push_back(
        MessagePart::tool("call_2", "deploy", read_args())
            .set_error("[отказ] пользователь не разрешил"));
    s.messages.push_back(refused);

    Message service;
    service.id = "msg_000000000014";
    service.role = kRoleAssistant;
    service.parts.push_back(MessagePart::step_finish("step_1"));
    json::JsonValue files = json::JsonValue::array();
    files.push_back(json::JsonValue("core/engine.cpp"));
    service.parts.push_back(MessagePart::patch("a1b2c3", files));
    service.parts.push_back(MessagePart::retry(2, 1500));
    service.parts.push_back(
        MessagePart::compaction("сводка", {"msg_000000000010"}));
    service.parts.push_back(
        MessagePart::subtask("ses_000000000002", "wp_explore"));
    s.messages.push_back(service);
    return s;
}

void compare(const MessagePart& a, const MessagePart& b) {
    ASSERT_EQ(part_kind_name(a.kind()), std::string(part_kind_name(b.kind())));
    ASSERT_EQ(a.text(), b.text());
    ASSERT_EQ(a.call_id(), b.call_id());
    ASSERT_EQ(a.tool_name(), b.tool_name());
    ASSERT_EQ(a.raw_call(), b.raw_call());
    ASSERT_EQ(tool_state_name(a.state()), std::string(tool_state_name(b.state())));
    ASSERT_EQ(a.output().output, b.output().output);
    ASSERT_EQ(a.output().title, b.output().title);
    ASSERT_EQ(a.output().truncated, b.output().truncated);
    ASSERT_EQ(a.output().metadata.dump(), b.output().metadata.dump());
    ASSERT_EQ(a.error(), b.error());
    ASSERT_EQ(a.step_name(), b.step_name());
    ASSERT_EQ(a.snapshot_hash(), b.snapshot_hash());
    ASSERT_EQ(a.files().dump(), b.files().dump());
    ASSERT_EQ(a.task_id(), b.task_id());
    ASSERT_EQ(a.subagent(), b.subagent());
    ASSERT_EQ(std::to_string(a.attempt()), std::to_string(b.attempt()));
    ASSERT_EQ(std::to_string(a.next_attempt_in_ms()),
              std::to_string(b.next_attempt_in_ms()));
    ASSERT_EQ(a.replaced_ids().size(), b.replaced_ids().size());
    for (size_t i = 0; i < a.replaced_ids().size(); ++i) {
        ASSERT_EQ(a.replaced_ids()[i], b.replaced_ids()[i]);
    }
}

} // namespace

TEST(session_file_roundtrip_keeps_every_part) {
    const SessionFile original = full_session();
    const json::JsonValue json = SessionArchive::to_json(original);

    SessionFile loaded;
    std::string error;
    std::vector<std::string> warnings;
    ASSERT_TRUE(SessionArchive::from_json(json, loaded, &error, &warnings));
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(warnings.empty());
    ASSERT_EQ(loaded.session_id, original.session_id);
    ASSERT_EQ(loaded.messages.size(), original.messages.size());
    for (size_t i = 0; i < original.messages.size(); ++i) {
        const Message& a = original.messages[i];
        const Message& b = loaded.messages[i];
        ASSERT_EQ(a.id, b.id);
        ASSERT_EQ(a.role, b.role);
        ASSERT_EQ(a.parent_id, b.parent_id);
        ASSERT_EQ(a.parts.size(), b.parts.size());
        for (size_t j = 0; j < a.parts.size(); ++j) compare(a.parts[j], b.parts[j]);
    }
    /* И восстановленная сессия рендерится в ту же реплику модели, что и
     * до записи: иначе resume незаметно меняет то, что увидит модель. */
    ASSERT_EQ(to_model_messages(loaded.messages).size(),
              to_model_messages(original.messages).size());
    for (size_t i = 0; i < to_model_messages(loaded.messages).size(); ++i) {
        const std::vector<ModelMessage> a = to_model_messages(original.messages);
        const std::vector<ModelMessage> b = to_model_messages(loaded.messages);
        ASSERT_EQ(a[i].role, b[i].role);
        ASSERT_EQ(a[i].content, b[i].content);
    }
}

TEST(session_file_roundtrip_survives_awkward_text) {
    /* Ровно то, что ломало старый сканер (D10): '}' внутри кода, кавычки,
     * переводы строк и не-ASCII. */
    SessionFile s;
    s.session_id = "ses_000000000001";
    Message m;
    m.id = "msg_000000000001";
    m.role = kRoleUser;
    m.parts.push_back(MessagePart::text(
        "function f() {\n  return [\"a}b\", 'c\\\\d'];\n}\n"
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \"мир\""));
    s.messages.push_back(m);

    SessionFile loaded;
    std::string error;
    ASSERT_TRUE(SessionArchive::from_json(SessionArchive::to_json(s), loaded,
                                        &error, nullptr));
    ASSERT_EQ(loaded.messages.size(), size_t(1));
    ASSERT_EQ(loaded.messages[0].parts[0].text(), m.parts[0].text());
    /* JSON сохранил текст байт в байт, и это валидный UTF-8. */
    ASSERT_TRUE(text::is_valid_utf8(loaded.messages[0].parts[0].text()));
}

TEST(session_file_refuses_a_newer_version_instead_of_half_reading_it) {
    /* Файл из более новой версии плагина. Частичное чтение выбросило бы
     * незнакомое, а следующая запись затела бы файл своим — потеря
     * истории безвозвратно и без единого сообщения. */
    json::JsonValue root = json::JsonValue::object();
    root.set("version", kSessionFileVersion + 1);
    json::JsonValue msgs = json::JsonValue::array();
    root.set("messages", msgs);

    SessionFile out;
    std::string error;
    ASSERT_FALSE(SessionArchive::from_json(root, out, &error, nullptr));
    ASSERT_TRUE(!error.empty());
    ASSERT_TRUE(out.messages.empty());
}

TEST(session_file_reports_what_it_did_not_understand) {
    /* Незнакомое внутри знакомого файла: сессия выживает, потеря видима. */
    const std::string text =
        R"({"version":1,"session":"ses_000000000001","messages":[)"
        R"({"id":"msg_000000000001","role":"user","parts":[)"
        R"({"kind":"text","text":"задача"},)"
        R"({"kind":"quantum_flux","text":"что-то из будущего"},)"
        R"({"kind":"tool","call_id":"call_0","name":"bash","state":"triped"},)"
        R"({"kind":"text","text":"ещё текст"}]}]})";

    json::JsonValue root;
    ASSERT_TRUE(json::JsonValue::parse(text, root));
    SessionFile out;
    std::string error;
    std::vector<std::string> warnings;
    ASSERT_TRUE(SessionArchive::from_json(root, out, &error, &warnings));
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(out.messages.size(), size_t(1));
    /* Часть неизвестного вида пропущена, остальные на месте. */
    ASSERT_EQ(out.messages[0].parts.size(), size_t(3));
    /* Обе потери названы поимённо: «предупреждение вообще есть» —
     * недостаточная проверка, оно пришло бы и от второй потери. */
    const auto mentions = [&warnings](const std::string& what) {
        return std::any_of(warnings.begin(), warnings.end(),
                           [&what](const std::string& w) {
                               return w.find(what) != std::string::npos;
                           });
    };
    ASSERT_TRUE(mentions("quantum_flux"));
    ASSERT_TRUE(mentions("triped"));
    /* Неизвестное состояние трактуется как «не начат»: вызов останется
     * открытым и попадёт под условие завершения (И5.8), а не будет молча
     * считаться выполненным. */
    ASSERT_EQ(out.messages[0].parts[1].state(), ToolState::Pending);
    ASSERT_TRUE(out.messages[0].parts[1].is_open());
}

TEST(session_file_rejects_broken_json_with_a_reason) {
    const std::string broken = R"({"version":1,"messages":[{"role":)";

    const fs::path dir = tmp_dir();
    const fs::path file = dir / "broken.json";
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << broken;
    }
    SessionFile out;
    std::string error;
    ASSERT_FALSE(SessionArchive::load(file.string(), out, &error, nullptr));
    ASSERT_TRUE(!error.empty());

    /* Пустой файл, не объект, нет messages — тоже отказ, а не пустая
     * сессия: иначе следующая запись затрёт историю молча. */
    const fs::path empty_file = dir / "empty.json";
    { std::ofstream f(empty_file, std::ios::binary | std::ios::trunc); }
    ASSERT_FALSE(SessionArchive::load(empty_file.string(), out, &error, nullptr));
    ASSERT_TRUE(!error.empty());

    const fs::path arr_file = dir / "array.json";
    {
        std::ofstream f(arr_file, std::ios::binary | std::ios::trunc);
        f << R"([{"role":"user","content":"старая сессия"}])";
    }
    ASSERT_FALSE(SessionArchive::load(arr_file.string(), out, &error, nullptr));
    ASSERT_TRUE(!error.empty());
    ASSERT_TRUE(out.messages.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(session_save_is_atomic_and_keeps_the_previous_file_on_failure) {
    const fs::path dir = tmp_dir();
    const std::string path = (dir / "wp_coder" / "sessions" / "ses_1.json").string();

    SessionFile first;
    first.session_id = "ses_000000000001";
    Message m;
    m.id = "msg_000000000001";
    m.role = kRoleUser;
    m.parts.push_back(MessagePart::text("первая сессия"));
    first.messages.push_back(m);

    std::string error;
    ASSERT_TRUE(SessionArchive::save(path, first, &error));
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(fs::exists(path));
    /* Каталог создался сам, временного файла не осталось. */
    ASSERT_TRUE(fs::is_directory(fs::path(path).parent_path()));
    ASSERT_FALSE(fs::exists(path + ".tmp"));

    /* Неудачная запись не должна уничтожить предыдущую: путь в
     * несуществующий каталог, который нельзя создать. */
    const std::string bad_path = "/proc/wp_coder/нельзя/ses_1.json";
    SessionFile second = first;
    second.messages[0].parts[0] = MessagePart::text("вторая сессия");
    ASSERT_FALSE(SessionArchive::save(bad_path, second, &error));
    ASSERT_TRUE(!error.empty());

    SessionFile loaded;
    ASSERT_TRUE(SessionArchive::load(path, loaded, &error, nullptr));
    ASSERT_EQ(loaded.messages.size(), size_t(1));
    ASSERT_EQ(loaded.messages[0].parts[0].text(), std::string("первая сессия"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(session_file_path_and_current_file) {
    const std::string p =
        SessionArchive::file_path("/data", "ses_000000000007");
    ASSERT_EQ(p, std::string("/data/wp_coder/sessions/ses_000000000007.json"));
    /* Без каталога или без идентификатора пути нет: писать некуда, и
     * молчаливый путь «куда-то» хуже отказа. */
    ASSERT_TRUE(SessionArchive::file_path("", "ses_1").empty());
    ASSERT_TRUE(SessionArchive::file_path("/data", "").empty());
    ASSERT_TRUE(SessionArchive::current_file("").empty());
    ASSERT_TRUE(SessionArchive::current_file("/нет/такого/каталога").empty());

    /* Текущая сессия — самая свежая по времени изменения, а не первая по
     * имени: иначе resume открыл бы не тот диалог. */
    const fs::path base = tmp_dir();          /* один каталог на тест */
    const std::string data_dir = base.string();
    const fs::path dir = base / "wp_coder" / "sessions";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ASSERT_TRUE(SessionArchive::current_file(data_dir).empty());
    const std::string older = (dir / "ses_000000000001.json").string();
    const std::string newer = (dir / "ses_000000000002.json").string();
    SessionFile s;
    s.session_id = "ses_000000000001";
    std::string error;
    ASSERT_TRUE(SessionArchive::save(older, s, &error));
    ASSERT_EQ(SessionArchive::current_file(data_dir), older);
    ASSERT_TRUE(SessionArchive::save(newer, s, &error));
    /* Порядок по времени: новый файл выигрывает. Сравниваем именно его, а
     * не «любой из двух»: иначе проверка прошла бы и при выборе по
     * имени. */
    ASSERT_EQ(SessionArchive::current_file(data_dir), newer);
    /* Файл, который не .json, текущей сессией не считается. */
    {
        std::ofstream f((dir / "notes.txt").string(), std::ios::binary);
        f << "не сессия";
    }
    ASSERT_EQ(SessionArchive::current_file(data_dir), newer);
    std::error_code rm;
    fs::remove_all(base, rm);
}

/* Загрузка обязана поднять счётчик идентификаторов: счётчик процесса
 * стартует с нуля, а в файле уже есть msg_000000000412. Иначе первый
 * новый ход получил бы занятый id, и два сообщения слиплись бы в одно —
 * молча, потому что обе части выглядели бы валидно (И5.5). */
TEST(session_load_advances_the_id_sequence) {
    ids().reset();
    const SessionFile s = full_session();
    SessionFile loaded;
    std::string error;
    ASSERT_TRUE(SessionArchive::from_json(SessionArchive::to_json(s), loaded,
                                          &error, nullptr));
    const std::string fresh = ids().next_msg();
    /* Больше любого идентификатора из файла. */
    ASSERT_TRUE(id_number(fresh) > 14);
    ASSERT_TRUE(id_number(fresh) > id_number(loaded.session_id));
    for (const Message& m : loaded.messages) {
        ASSERT_TRUE(id_number(fresh) > id_number(m.id));
    }
    ids().reset();
}

/* Равные времена изменения — норма для ФС с секундной точностью. Развязка
 * обязана быть по номеру идентификатора, а не по имени: ses_9 при
 * сравнении строк «новее» ses_10, и resume открыл бы старый диалог. */
TEST(session_current_file_breaks_ties_by_id_not_by_name) {
    const fs::path base = tmp_dir();
    const std::string data_dir = base.string();
    const fs::path dir = base / "wp_coder" / "sessions";
    std::error_code ec;
    fs::create_directories(dir, ec);

    SessionFile s;
    s.session_id = "ses_000000000001";
    std::string error;
    const std::string tenth = (dir / "ses_000000000010.json").string();
    const std::string ninth = (dir / "ses_000000000009.json").string();
    ASSERT_TRUE(SessionArchive::save(tenth, s, &error));
    ASSERT_TRUE(SessionArchive::save(ninth, s, &error));
    /* Записи подряд: времена, скорее всего, равны, и выбор обязан быть
     * детерминированным. */
    ASSERT_EQ(SessionArchive::current_file(data_dir), tenth);

    std::error_code rm;
    fs::remove_all(base, rm);
}

/* Файл мог остаться битым после падения на старой версии. Отказ читать
 * такую сессию молча хуже, чем потеря одного символа: пользователь хотя
 * бы увидит историю. */
TEST(session_load_sanitizes_a_broken_byte_in_the_file) {
    const fs::path base = tmp_dir();
    const std::string path =
        (base / "wp_coder" / "sessions" / "ses_000000000001.json").string();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "{\"version\":1,\"session\":\"ses_000000000001\",\"messages\":[{"
             "\"id\":\"msg_000000000001\",\"role\":\"user\",\"parts\":"
             "[{\"kind\":\"text\",\"text\":\"до\xFFпосле\"}]}]}";
    }
    SessionFile out;
    std::string error;
    ASSERT_TRUE(SessionArchive::load(path, out, &error, nullptr));
    ASSERT_EQ(out.messages.size(), size_t(1));
    const std::string text = out.messages[0].parts[0].text();
    ASSERT_TRUE(text::is_valid_utf8(text));
    ASSERT_TRUE(text.find("до") == 0);
    ASSERT_TRUE(text.find("после") != std::string::npos);
    std::error_code rm;
    fs::remove_all(base, rm);
}
