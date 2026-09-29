// session_store.cpp — сессия на диске (И5.6).

#include "session_store.h"
#include "json_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace coder {
namespace {

json::JsonValue output_to_json(const ToolOutput& out) {
    json::JsonValue v = json::JsonValue::object();
    v.set("title", out.title);
    v.set("output", out.output);
    if (!out.metadata.is_null()) v.set("metadata", out.metadata);
    if (out.truncated) v.set("truncated", true);
    return v;
}

ToolOutput output_from_json(const json::JsonValue& v) {
    ToolOutput out;
    if (!v.is_object()) return out;
    out.title = v.get_string("title", "");
    out.output = v.get_string("output", "");
    if (v.has("metadata")) out.metadata = v.get("metadata");
    out.truncated = v.get_bool("truncated", false);
    return out;
}

json::JsonValue part_to_json(const MessagePart& p) {
    json::JsonValue v = json::JsonValue::object();
    v.set("kind", std::string(p.kind_name()));
    switch (p.kind()) {
        case PartKind::Text:
        case PartKind::Reasoning:
            v.set("text", p.text());
            break;
        case PartKind::Tool:
            v.set("call_id", p.call_id());
            v.set("name", p.tool_name());
            if (!p.args().is_null()) v.set("args", p.args());
            if (!p.raw_call().empty()) v.set("raw", p.raw_call());
            v.set("state", std::string(p.state_name()));
            /* Метка очищенного вывода переживает перезагрузку: без неё
             * восстановленная сессия вернула бы в контекст текст, который
             * прореживание убрало, и переполнение вернулось бы молча,
             * ровно по той причине, по которой его чинили. */
            if (p.output_cleared()) v.set("output_cleared", true);
            if (!p.output().output.empty() || !p.output().title.empty() ||
                !p.output().metadata.is_null() || p.output().truncated) {
                v.set("output", output_to_json(p.output()));
            }
            if (!p.error().empty()) v.set("error", p.error());
            break;
        case PartKind::StepStart:
        case PartKind::StepFinish:
            v.set("step", p.step_name());
            break;
        case PartKind::Patch:
            v.set("hash", p.snapshot_hash());
            if (!p.files().is_null()) v.set("files", p.files());
            break;
        case PartKind::Retry:
            v.set("attempt", p.attempt());
            v.set("next_ms", p.next_attempt_in_ms());
            break;
        case PartKind::Compaction: {
            v.set("text", p.text());
            json::JsonValue ids = json::JsonValue::array();
            for (const std::string& id : p.replaced_ids()) {
                ids.push_back(json::JsonValue(id));
            }
            v.set("replaced", ids);
            break;
        }
        /* Реплика автопродолжения: вид пишется сам (part_kind_name), и
         * остаётся только текст. Иначе после перезагрузки сессии она
         * стала бы обычной репликой пользователя, и ни сводка, ни UI не
         * смогли бы отличить слова плагина от слов человека. */
        case PartKind::CompactionContinue:
            v.set("text", p.text());
            break;
        case PartKind::Subtask:
            v.set("task_id", p.task_id());
            v.set("subagent", p.subagent());
            break;
    }
    return v;
}

/* Состояние вызова по строке. found=false — неизвестное имя. */
bool tool_state_from_name(const std::string& name, ToolState& out) {
    if (name == "pending")        { out = ToolState::Pending;   return true; }
    if (name == "running")        { out = ToolState::Running;   return true; }
    if (name == "completed")      { out = ToolState::Completed; return true; }
    if (name == "error")          { out = ToolState::Error;     return true; }
    return false;
}

/* Разбор части с добавлением в список. false — часть не понята
 * (неизвестный вид) и в список НЕ добавлена.
 *
 * Именно добавление, а не «разобрать в объект»: у MessagePart нет
 * конструктора по умолчанию намеренно (у части всегда есть вид), и
 * заводить его ради парсера значило бы разрешить собирать части, у
 * которых вида нет. */
bool append_part_from_json(const json::JsonValue& v,
                           std::vector<MessagePart>& out,
                           std::vector<std::string>* warnings) {
    if (!v.is_object()) return false;
    const std::string kind = v.get_string("kind", "");
    const auto warn = [&](const std::string& what) {
        if (warnings) warnings->push_back(what);
    };

    if (kind == "text") {
        out.push_back(MessagePart::text(v.get_string("text", "")));
        return true;
    }
    if (kind == "reasoning") {
        out.push_back(MessagePart::reasoning(v.get_string("text", "")));
        return true;
    }
    if (kind == "step_start") {
        out.push_back(MessagePart::step_start(v.get_string("step", "")));
        return true;
    }
    if (kind == "step_finish") {
        out.push_back(MessagePart::step_finish(v.get_string("step", "")));
        return true;
    }
    if (kind == "patch") {
        json::JsonValue files = v.has("files") ? v.get("files")
                                               : json::JsonValue::array();
        out.push_back(MessagePart::patch(v.get_string("hash", ""), files));
        return true;
    }
    if (kind == "retry") {
        out.push_back(MessagePart::retry(
            static_cast<int>(v.get_int("attempt", 0)),
            static_cast<int>(v.get_int("next_ms", 0))));
        return true;
    }
    if (kind == "compaction") {
        std::vector<std::string> replaced;
        if (v.has("replaced")) {
            const json::JsonValue& arr = v.get("replaced");
            for (size_t i = 0; i < arr.size(); ++i) {
                replaced.push_back(arr.at(i).as_string());
            }
        }
        out.push_back(MessagePart::compaction(v.get_string("text", ""),
                                              replaced));
        return true;
    }
    if (kind == "compaction_continue") {
        /* Текст из файла НЕ читается, а берётся из kCompactionContinueText:
         * это протокольная строка, и владелец у неё один. Иначе правильная
         * копия и рассинхронизированная выглядели бы одинаково, а при
         * пустом тексте на выходе получилась бы реплика пользователя без
         * содержимого. */
        out.push_back(MessagePart::compaction_continue());
        return true;
    }
    if (kind == "subtask") {
        out.push_back(MessagePart::subtask(v.get_string("task_id", ""),
                                            v.get_string("subagent", "")));
        return true;
    }
    if (kind == "tool") {
        json::JsonValue args = v.has("args") ? v.get("args")
                                            : json::JsonValue::object();
        MessagePart part = MessagePart::tool(
            v.get_string("call_id", ""), v.get_string("name", ""), args,
            v.get_string("raw", ""));
        const std::string state_name = v.get_string("state", "pending");
        ToolState state = ToolState::Pending;
        if (!tool_state_from_name(state_name, state)) {
            /* Неизвестное состояние трактуем как «не начат»: так вызов
             * останется в открытых и попадёт под условие завершения
             * (И5.8), а не будет молча считаться выполненным. */
            state = ToolState::Pending;
            warn("неизвестное состояние вызова «" + state_name +
                 "» — считаем pending");
        }
        switch (state) {
            case ToolState::Running:
                part.set_running();
                break;
            case ToolState::Completed:
                part.set_result(output_from_json(v.get("output")));
                break;
            case ToolState::Error:
                part.set_error(v.get_string("error", "ошибка вызова"));
                break;
            case ToolState::Pending:
                break;
        }
        /* Метка ставится ПОСЛЕ состояния: clear_output() работает только на
         * завершённом вызове, а до разбора состояния часть ещё «не
         * начата». Раньше метка молча терялась бы — файл её содержал бы,
         * а в истории её не было бы. */
        if (v.has("output_cleared")) part.clear_output();
        out.push_back(std::move(part));
        return true;
    }

    warn("неизвестный вид части «" + kind + "» — часть пропущена");
    return false;
}

void set_error(std::string* error, const std::string& what) {
    if (error) *error = what;
}

} // namespace

json::JsonValue SessionArchive::to_json(const SessionFile& session) {
    json::JsonValue root = json::JsonValue::object();
    root.set("version", session.version);
    root.set("session", session.session_id);
    json::JsonValue msgs = json::JsonValue::array();
    for (const Message& m : session.messages) {
        json::JsonValue mv = json::JsonValue::object();
        mv.set("id", m.id);
        mv.set("role", m.role);
        if (!m.parent_id.empty()) mv.set("parent_id", m.parent_id);
        json::JsonValue parts = json::JsonValue::array();
        for (const MessagePart& p : m.parts) {
            parts.push_back(part_to_json(p));
        }
        mv.set("parts", parts);
        msgs.push_back(std::move(mv));
    }
    root.set("messages", std::move(msgs));
    return root;
}

bool SessionArchive::from_json(const json::JsonValue& value, SessionFile& out,
                             std::string* error,
                             std::vector<std::string>* warnings) {
    if (!value.is_object()) {
        set_error(error, "корень файла сессии — не объект");
        return false;
    }
    const long long version = value.get_int("version", 0);
    if (version > kSessionFileVersion) {
        /* Файл из более новой версии плагина. Читать его «по частям» —
         * значит выбросить то, чего мы не знаем, и записать поверх
         * свой файл, то есть испортить данные безвозвратно. */
        set_error(error, "файл сессии новее плагина (формат " +
                             std::to_string(version) + ", поддерживается " +
                             std::to_string(kSessionFileVersion) + ")");
        return false;
    }
    if (version < kSessionFileVersion && warnings) {
        warnings->push_back("файл сессии записан в старом формате (" +
                            std::to_string(version) + ")");
    }

    out = SessionFile();
    out.version = kSessionFileVersion;
    out.session_id = value.get_string("session", "");
    if (!value.has("messages")) {
        set_error(error, "в файле сессии нет списка сообщений");
        return false;
    }
    const json::JsonValue& msgs = value.get("messages");
    if (!msgs.is_array()) {
        set_error(error, "поле messages — не массив");
        return false;
    }

    for (size_t i = 0; i < msgs.size(); ++i) {
        const json::JsonValue& mv = msgs.at(i);
        if (!mv.is_object()) {
            if (warnings) {
                warnings->push_back("сообщение " + std::to_string(i) +
                                    " не объект — пропущено");
            }
            continue;
        }
        Message m;
        m.id = mv.get_string("id", "");
        /* Роль приходит строкой и остаётся строкой: неизвестная роль —
         * это данные, а не повод их выбросить. */
        m.role = mv.get_string("role", "");
        m.parent_id = mv.get_string("parent_id", "");
        if (mv.has("parts")) {
            const json::JsonValue& parts = mv.get("parts");
            if (parts.is_array()) {
                for (size_t j = 0; j < parts.size(); ++j) {
                    append_part_from_json(parts.at(j), m.parts, warnings);
                }
            } else if (warnings) {
                warnings->push_back("у сообщения " + m.id +
                                    " parts — не массив, частей нет");
            }
        }
        out.messages.push_back(std::move(m));
    }

    /* Идентификаторы подняли счётчик: иначе первый новый ход получил бы
     * занятый id (И5.5), и два сообщения слиплись бы в одно — молча. */
    if (!out.session_id.empty()) ids().observe(out.session_id);
    for (const Message& m : out.messages) ids().observe(m.id);
    return true;
}

std::string SessionArchive::file_path(const std::string& data_dir,
                                    const std::string& session_id) {
    if (data_dir.empty() || session_id.empty()) return std::string();
    return data_dir + "/wp_coder/sessions/" + session_id + ".json";
}

std::string SessionArchive::current_file(const std::string& data_dir) {
    if (data_dir.empty()) return std::string();
    const fs::path dir = fs::path(data_dir) / "wp_coder" / "sessions";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::string();

    fs::path newest;
    fs::file_time_type newest_time{};
    unsigned long long newest_id = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        const auto t = fs::last_write_time(entry.path(), ec);
        if (ec) continue;

        /* Выбор: сначала время изменения, но при РАВНОМ времени — по
         * номеру идентификатора.
         *
         * На равных временах одного выбора «считать первым» мало:
         * точность времени файла бывает в секунду (ext4 в контейнерах), и
         * две сессии, записанные подряд, получают одно время. Тогда выбор
         * был бы произвольным, и resume мог бы открыть не тот диалог —
         * молча. Номер идентификатора монотонен (И5.5), поэтому как
         * развязка он точен: позже созданная сессия имеет больший номер.
         *
         * Порядок по ИМЕНИ как развязка не годился бы: имя — строка, и
         * ses_9 «новее» ses_10. */
        const unsigned long long id = id_number(
            entry.path().stem().string());
        const bool better_time = newest.empty() || t > newest_time;
        const bool same_time = !newest.empty() && t == newest_time;
        if (better_time || (same_time && id > newest_id)) {
            newest = entry.path();
            newest_time = t;
            newest_id = id;
        }
    }
    return newest.empty() ? std::string() : newest.string();
}

bool SessionArchive::save(const std::string& path, const SessionFile& session,
                        std::string* error) {
    if (path.empty()) {
        set_error(error, "путь к файлу сессии не задан");
        return false;
    }
    std::error_code ec;
    const fs::path fs_path(path);
    fs::create_directories(fs_path.parent_path(), ec);
    if (ec) {
        set_error(error, "не удалось создать каталог: " + ec.message());
        return false;
    }

    /* Атомарно: временный файл рядом, затем rename. Обрезанный файл после
     * падения читался бы как пустая сессия и затерел бы историю при
     * следующем сохранении (D19). */
    const fs::path tmp = fs_path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            set_error(error, "не удалось открыть файл на запись: " + tmp.string());
            return false;
        }
        f << to_json(session).dump(2);
        f.flush();
        if (!f) {
            f.close();
            std::error_code rm;
            fs::remove(tmp, rm);
            set_error(error, "запись файла сессии оборвалась");
            return false;
        }
    }
    fs::rename(tmp, fs_path, ec);
    if (ec) {
        std::error_code rm;
        fs::remove(tmp, rm);
        set_error(error, "не удалось переименовать файл сессии: " + ec.message());
        return false;
    }
    return true;
}

bool SessionArchive::load(const std::string& path, SessionFile& out,
                        std::string* error,
                        std::vector<std::string>* warnings) {
    if (path.empty()) {
        set_error(error, "путь к файлу сессии не задан");
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        set_error(error, "файл сессии не найден: " + path);
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    /* Файл мог остаться битым (обрыв записи на старой версии, диск).
     * Разбор битого UTF-8 ничем не лучше отказа: чистим и читаем. */
    content = text::sanitize_utf8(content);
    if (content.empty()) {
        set_error(error, "файл сессии пуст: " + path);
        return false;
    }
    json::JsonValue root;
    std::string parse_error;
    if (!json::JsonValue::parse(content, root, &parse_error)) {
        set_error(error, "файл сессии повреждён (" + parse_error + "): " + path);
        return false;
    }
    return from_json(root, out, error, warnings);
}

bool SessionArchive::remove(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::remove(path, ec);
    return !ec;
}

} // namespace coder
