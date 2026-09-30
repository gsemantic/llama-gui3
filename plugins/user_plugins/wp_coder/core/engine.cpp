// engine.cpp — AI-кодер с разбиением run_task на компоненты (Фаза D1)

#include "agent_components.h"
#include "agent_registry.h"
#include "prompts.h"
#include "shell.h"
#include "engine.h"
#include "session_store.h"
#include "file_lock.h"
#include "limits.h"
#include "json_utils.h"

#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <iterator>
#include <filesystem>

namespace coder {

/* Бюджет символов на историю сессии — единый источник: core/limits.h (4.5). */
using limits::kSessionBudget;

namespace fs = std::filesystem;

/*
 * Engine — глобальный singleton
 * ====================================================================== */

Engine& Engine::instance() {
    static Engine eng;
    return eng;
}

void Engine::init(HostCallbacks callbacks) {
    cb_ = std::move(callbacks);
    /* Мост системы разрешений в состояние и лог: без него PermissionEngine
     * умеет решать, но не умеет спросить (И2.5). */
    permissions_.bind(&state_, [this](const std::string& text) {
        push_event(AgentEvent::Tool, text);
    });
    load_settings();
}

void Engine::start() {
    state_.worker = std::thread(&Engine::worker_main, this);
}

void Engine::stop() {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.shutting_down = true;
        state_.permission_cv.notify_all();
        state_.cv.notify_all();
    }
    /* Вне лока: cancel_all() берёт свой mtx_, а брать его поверх
     * state_.mtx здесь нельзя — PermissionEngine ходит в state_.mtx
     * короткими захватами, и обратный порядок означал бы дедлок. */
    permissions_.cancel_all();
    if (state_.worker.joinable()) state_.worker.join();
    /* Resume (5.2): сохраняем последний диалог перед выключением. */
    save_session();
}

void Engine::submit(const std::string& prompt) {
    /* Новая задача — снимаем отмену разрешений от прошлой. Иначе после
     * «стоп» движок задавал бы мгновенные отказы на любой вопрос до
     * конца сессии. Вне лока: cancel_all/clear_cancel берут свой mtx_,
     * и брать его поверх state_.mtx нельзя (порядок блокировок). */
    permissions_.clear_cancel();
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        bool resume = state_.preserve_session;
        state_.preserve_session = false;
        if (resume) {
            /* Возобновление после паузы на разрешение: не модифицируем сессию. */
        } else if (state_.continue_conversation && !state_.session.empty()) {
            /* Продолжение сессии: добавляем сообщение к существующему контексту.
             * План и предыдущие результаты сохраняются — модель знает историю. */
            state_.session.push_back(Message::user(prompt));
            std::cerr << "[wp_coder] submit: continue session, msgs="
                      << state_.session.size() << std::endl;
        } else {
            /* Новая задача: очищаем сессию и начинаем заново. */
            state_.session.clear();
            state_.session.push_back(Message::user(prompt));
            /* И4.6: план прошлой задачи в промпте новой только сбивает —
             * модель сверялась бы с чужими пунктами. */
            state_.todos.clear();
            state_.prompt_dirty = true;
            std::cerr << "[wp_coder] submit: new session" << std::endl;
        }
        state_.last_agent_task = prompt;
        state_.response_ready = false;
        state_.last_response.clear();
        state_.abort_requested.store(false);
        /* И6.7: СВОЙ токен на ход. Переиспользовать общий нельзя: второе
         * нажатие «стоп» отменило бы следующую задачу, и отмену нельзя
         * было бы снять вообще. */
        state_.turn_abort = std::make_shared<AbortToken>();
        state_.inbox.push(prompt);
        state_.cv.notify_all();
        std::cerr << "[wp_coder] submit: inbox_size=" << state_.inbox.size()
                  << " state=" << agent_state_name(state_.state) << std::endl;
    }
}

void Engine::request_abort() {
    /* Снаружи лока — см. комментарий в stop(). Именно этот вызов
     * вытаскивает worker из ожидания разрешения: иначе «стоп» работал
     * бы не на том шаге, где агент висит на вопросе пользователю. */
    permissions_.cancel_all();
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.abort_requested.store(true);
        /* Токен отменяется здесь, а не читается инструментами по флагу:
         * отмена должна БУДИТЬ ожидание, а poll каждые 100 мс в цикле
         * команды — это отмена, которой нельзя дождаться. */
        if (state_.turn_abort) state_.turn_abort->abort();
        /* Не переводим FSM здесь: wait()-предикаты включают abort_requested,
         * поток разбудится и AgentLoop/cleanup сам переведёт в Aborted. */
        state_.permission_cv.notify_all();
    }
}

void Engine::clear_session() {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session.clear();
        state_.recent_calls.clear();
        state_.last_agent_task.clear();
        state_.todos.clear();       /* И4.6: план относится к задаче */
        state_.prompt_dirty = true;
    }  /* mtx отпущен — push_event безопасен */
    /* Удаляем и сохранённую на диске сессию (resume, 5.2). Идентификатор
     * сбрасываем: следующая запись должна создать НОВЫЙ файл, а не
     * переписать старый (иначе след прежней сессии остался бы на диске
     * и «текущим» считался бы он). */
    std::string path = session_file_path();
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session_id.clear();
    }
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    push_event(AgentEvent::Status, "Сессия очищена — следующий запрос начнётся заново.");
}

/* Resume сессии (И5.6). Файл определяется идентификатором сессии, а пока
 * он не задан — самым свежим файлом в каталоге (SessionArchive::current_file).
 * Иначе после перезапуска агент не нашёл бы свою же историю. */
std::string Engine::session_file_path() const {
    if (!cb_.path_data_dir) return "";
    const std::string dir = cb_.path_data_dir();
    if (dir.empty()) return "";
    std::string id;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        id = state_.session_id;
    }
    if (!id.empty()) {
        const std::string p = SessionArchive::file_path(dir, id);
        if (!p.empty()) return p;
    }
    return SessionArchive::current_file(dir);
}

void Engine::save_session() {
    const std::string dir = cb_.path_data_dir ? cb_.path_data_dir() : "";
    if (dir.empty()) return;

    std::vector<Message> snap;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        snap = state_.session;
        session_id = state_.session_id;
    }
    if (snap.empty()) return;

    /* Идентификатор сессии выдаётся при первой записи и дальше тот же:
     * новый id означал бы новый файл, то есть потерю resume. */
    if (session_id.empty()) {
        session_id = ids().next_session();
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session_id = session_id;
    }

    /* И5.7: моста больше нет. Сессия ИСТОРИЯ структуры, и в файл уходит
     * та же структура — с теми же идентификаторами, частями, parent_id и
     * состоянием вызовов. До И5.7 здесь был копирующий мост
     * ChatMsg → Message, который терял всё, чего в строке не было, и
     * выдавал новые идентификаторы при каждом сохранении: два сообщения
     * с одним parent_id в файле выглядели бы валидно и разъехались бы. */
    SessionFile file;
    file.session_id = session_id;
    file.messages = std::move(snap);

    const std::string path = SessionArchive::file_path(dir, session_id);
    std::string error;
    if (!SessionArchive::save(path, file, &error)) {
        std::cerr << "[wp_coder] session: не удалось сохранить сессию: "
                  << error << std::endl;
    }
}

void Engine::load_session() {
    const std::string dir = cb_.path_data_dir ? cb_.path_data_dir() : "";
    if (dir.empty()) return;
    const std::string path = session_file_path();
    if (path.empty()) return;

    SessionFile file;
    std::string error;
    std::vector<std::string> warnings;
    if (!SessionArchive::load(path, file, &error, &warnings)) {
        /* Отказ читается как «сессии нет», но повреждённый файл убираем:
         * иначе следующая запись затрёт его, а пользователь так и не
         * увидит, что история пропала (D19). */
        std::cerr << "[wp_coder] session: " << error << std::endl;
        SessionArchive::remove(path);
        return;
    }
    for (const std::string& w : warnings) {
        std::cerr << "[wp_coder] session: " << w << std::endl;
    }

    /* И5.7: сообщения возвращаются в сессию как есть — с частями,
     * parent_id и состоянием вызовов. Разбирать их в строки (to_model_
     * messages) больше не нужно: результат вызова инструмента,
     * пришедший из resume, обязан остаться результатом, иначе следующий
     * шаг агента не поймёт, что уже сделано. */
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (!state_.session.empty()) return;      /* не затираем текущую */
    state_.session = file.messages;
    state_.session_id = file.session_id;
}

std::string Engine::wait_response(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_.mtx);
    if (timeout_ms > 0) {
        state_.response_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] {
            return state_.response_ready || state_.shutting_down;
        });
    } else {
        state_.response_cv.wait(lk, [this] {
            return state_.response_ready || state_.shutting_down;
        });
    }
    if (state_.response_ready) {
        state_.response_ready = false;
        return std::move(state_.last_response);
    }
    request_abort();
    return "[ошибка] таймаут ожидания ответа агента";
}

/* ======================================================================
 * Сборка системного промпта
 * ====================================================================== */

ScopedAgentScope::ScopedAgentScope(Engine& engine, RunScope next)
    : engine_(engine) {
    {
        std::lock_guard<std::mutex> lk(engine_.state().mtx);
        previous_ = engine_.state().scope;
        engine_.state().scope = std::move(next);
    }
    /* Кэш сбрасывается ЗДЕСЬ, а не в compose: собранный промпт уже
     * виден в нём, и пока область меняется, кэш принадлежит прежнему
     * агенту. */
    engine_.invalidate_prompt_cache();
}

ScopedAgentScope::~ScopedAgentScope() {
    {
        std::lock_guard<std::mutex> lk(engine_.state().mtx);
        engine_.state().scope = std::move(previous_);
    }
    engine_.invalidate_prompt_cache();
}

RunScope Engine::scope_snapshot() const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    return state_.scope;
}

std::string Engine::build_system_prompt() const {
    if (!state_.prompt_dirty && !state_.cached_system_prompt.empty())
        return state_.cached_system_prompt;

    /* И8.7: кто выполняет ход. Читается ОДИН раз и до всех блоков, у
     * которых своё представление о наборе инструментов и плане, иначе
     * половина промпта собралась бы по правилам сессии, а половина — по
     * правилам агента. */
    std::shared_ptr<agent::Info> info;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        info = state_.scope.info;
    }
    const agent::Info* agent_rules = info.get();

    /* project_dir — ПЕРВЫМ, чтобы модель точно увидела корень проекта.
     * Даже при длинном кастомном промпте эта информация не потеряется. */
    std::string sys;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.project_dir.empty()) {
            sys = "## КОРНЕВОЙ КАТАЛОГ ПРОЕКТА\n"
                  "Корень: " + state_.project_dir + "\n"
                  "Все пути в инструментах — относительно этого каталога.\n\n";
        }
    }

    sys += state_.agent_system_prompt.empty()
        ? std::string(kBaseSystemPrompt)
        : state_.agent_system_prompt;

    /* И8.7: промпт агента — ДОПОЛНЕНИЕ к базовому, а не замена. Базовый
     * объясняет протокол вызовов инструментов, агентский — роль и
     * требования к результату (у субагента они свои: вернётся только
     * текст). Место — сразу за базовым, до промпта модуля и каталога
     * инструментов: иначе роль оказалась бы между описанием проекта и
     * протоколом, и модель читала бы её как часть каталога. */
    if (agent_rules && !agent_rules->prompt().empty()) {
        sys += "\n\n";
        sys += agent_rules->prompt();
    }

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.active_module.empty()) {
            const auto& modules = ModuleRegistry::instance().modules();
            for (const auto* mod : modules) {
                if (state_.active_module == mod->name && mod->get_system_prompt) {
                    const char* p = mod->get_system_prompt();
                    if (p && p[0]) {
                        sys += "\n\n";
                        sys += p;
                    }
                    break;
                }
            }
        }
    }

    /* Каталог инструментов, построенный из их JSON-схем (И1.5).
     *
     * Раньше здесь был ручной список базовых инструментов в prompts.h
     * плюс отдельный каталог «инструментов модулей» — два источника
     * истины, оба разошлись с кодом: bash не был описан (D2),
     * а 29 инструментов модулей не были видны модели вообще (D15).
     * Теперь описание, валидация аргументов и этот каталог читают одну
     * схему, поэтому дрейф документации невозможен по построению.
     *
     * И2.8: запрещённые правилами разрешений в каталог не попадают —
     * модель не тратит шаг на вызов, который всё равно отклонят.
     * И8.7: для субагента правила — его СОБСТВЕННЫЕ (agent::Info уже
     * сложил поверх них правила сессии), и фильтр тот же, что у
     * сессии, — одна функция на оба ответа (visible_tool_names). */
    {
        std::vector<ToolDef> all = ToolsRegistry::instance().defs();
        std::vector<std::string> visible;
        if (agent_rules) {
            visible = visible_tool_names(all, [agent_rules](const std::string& key) {
                return agent_rules->denies_whole_key(key);
            });
        } else {
            visible = permissions_.visible_tools(all);
        }
        std::string cat =
            ToolsRegistry::instance().build_tool_catalogue(visible);
        if (!cat.empty()) {
            sys += "\n\n";
            sys += cat;
        }
    }

    /* И4.6: план задачи — в системный промпт, а не в историю.
     *
     * В истории он жил бы до конца сессии и костенел: агент, дойдя до
     * пункта 4, продолжал бы сверяться с планом из пункта 1. В промпте он
     * один и всегда свежий, а кэш инвалидируется при каждом todowrite.
     *
     * И8.7: агенту, у которого план закрыт целиком (wp_explore —
     * `todo: * → запретить`, 8.6), блок НЕ показывается. В нём прямо
     * сказано «держи план в актуальном состоянии», а инструмента для
     * этого у агента нет: модель тратила бы шаг на вызов, который
     * отклонят. Тот же вопрос о запрете целиком, что и у каталога. */
    if (!agent_rules || !agent_rules->denies_whole_key("todo")) {
        std::string plan;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            for (const auto& t : state_.todos) {
                plan += "- [" + t.status + "] " + t.content;
                if (!t.id.empty()) plan += " (id: " + t.id + ")";
                if (t.priority != "medium") plan += " [" + t.priority + "]";
                plan += "\n";
            }
        }
        if (!plan.empty()) {
            sys += "\n\n## ПЛАН ЗАДАЧИ (todowrite)\n\n";
            sys += plan;
            sys +=
                "\nДержи план в актуальном состоянии: отмечай выполненное"
                " статусом completed, текущий пункт — in_progress. План"
                " показывается пользователю, и он им вмешивается.\n";
        }
    }

    sys += SkillsManager::instance().build_skills_prompt();

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (state_.mode == 1) sys += kModeResearch;
        else if (state_.mode == 2) sys += kModeReview;
    }

    state_.cached_system_prompt = sys;
    state_.prompt_dirty = false;
    return sys;
}

/* ======================================================================
 * Утилиты для Engine (обертки над component классами)
 * ====================================================================== */

void Engine::pending_apply(size_t idx) {
    /* И4.5: содержимое забираем под state_.mtx, а пишем уже БЕЗ него.
     *
     * Порядок блокировок в этом коде — общий, и он не может быть
     * произвольным: инструменты берут сначала файловый семафор
     * (file_lock::Guard), и только потом state_.mtx внутри
     * propose_write. Если бы здесь было наоборот — state_.mtx, а поверх
     * него файловый семафор, — два потока, взявшие по одной блокировке,
     * ждали бы друг друга вечно. А этот метод зовётся из UI-потока, то
     * есть второй поток здесь не теоретический. */
    PendingWrite p;
    std::string project;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (idx >= state_.pending.size()) return;
        p = state_.pending[idx];
        project = state_.project_dir;
        state_.pending.erase(state_.pending.begin() + idx);
    }
    std::string abs = p.path;
    if (!abs.empty() && abs[0] != '/' && !project.empty()) {
        abs = project + "/" + abs;
    }
    file_lock::Guard guard(abs);
    std::ofstream f(abs, std::ios::binary | std::ios::trunc);
    if (f) { f << p.content; f.close(); }
}

void Engine::pending_discard(size_t idx) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (idx >= state_.pending.size()) return;
    state_.pending.erase(state_.pending.begin() + idx);
}

std::string Engine::check_external_permission(const std::string& abs_path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    return gate.check(abs_path);
}

void Engine::permission_allow_once(const std::string& path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    gate.allow_once(path);
}

void Engine::permission_allow_always(const std::string& path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    gate.allow_always(path);
}

void Engine::permission_reject(const std::string& path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    gate.reject();
}

void Engine::permission_reply(uint64_t id, PermissionReply how) {
    permissions_.reply(id, how);
}

std::vector<PermissionRequest> Engine::permission_pending() const {
    return permissions_.pending();
}

/* FSM (2.3): переход состояния. Observer-событие публикуется только при
 * реальном изменении — не спамим лог повторными переходами. */
void Engine::set_state(AgentState s) {
    AgentState prev;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        prev = state_.state;
        state_.state = s;
    }
    if (prev != s)
        push_event(AgentEvent::Status,
            std::string("state: ") + agent_state_name(s));
    else
        state_.permission_cv.notify_all();
}

/* ======================================================================
 * События
 * ====================================================================== */

void Engine::push_event(AgentEvent::Kind k, const std::string& text) {
    bool forward = false;
    std::string chat_line;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.events.push_back({k, text});
        if (state_.events.size() > 500) state_.events.pop_front();
    }
    if (cb_.chat_event) {
        switch (k) {
            case AgentEvent::Status:
                chat_line = text;
                forward = true;
                break;
            case AgentEvent::Error:
                chat_line = "! " + text;
                forward = true;
                break;
            case AgentEvent::Assistant:
                chat_line = text;
                forward = true;
                break;
            case AgentEvent::Tool:
                chat_line = "🛠 " + text;
                forward = true;
                break;
            default:
                break;
        }
        if (forward && !chat_line.empty()) {
            if (chat_line.size() > 300) { chat_line = text::utf8_prefix(chat_line, 300); chat_line += "…"; }
            cb_.chat_event(chat_line);
        }
    }
}

/* ======================================================================
 * Настройки (загрузка/сохранение)
 * ====================================================================== */

static std::string setting_get(const HostCallbacks& cb, const std::string& key, const std::string& def) {
    if (!cb.settings_get) return def;
    std::string v = cb.settings_get(key, def);
    if (v.empty()) return def;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return v;
}

static void setting_set(const HostCallbacks& cb, const std::string& key, const std::string& value) {
    if (!cb.settings_set) return;
    cb.settings_set(key, "\"" + value + "\"");
}

void Engine::load_settings() {
    state_.project_dir      = setting_get(cb_, "wp_coder.project_dir", "");
    if (state_.project_dir.empty()) {
        std::cout << "[wp_coder] ВНИМАНИЕ: корневой каталог проекта не задан — "
                  << "агент не будет знать, где находится код" << std::endl;
    }
    /* И8.2: конфиг-агенты проекта из `<project>/.wpcode/agent/*.md`.
     *
     * Загрузка живёт здесь, а не в плагине при старте, потому что
     * каталог задаётся НАСТРОЙКОЙ проекта: у двух открытых проектов
     * наборы агентов разные, и агент, загруженный до смены проекта,
     * жил бы в чужом наборе. Отсутствие каталога — не ошибка
     * (load_agents_from_directory молчит о нём), а набор агентов
     * переживает перечитывание настроек: замена по имени сохраняет
     * прежнего агента, если новый файл не прочитался. */
    /* Встроенные агенты — ДО конфиг-агентов: тогда конфиг перекрывает
     * встроенного на его месте, а не сдвигает его в конец списка. */
    register_builtin_agents(AgentRegistry::instance());
    if (!state_.project_dir.empty()) {
        for (const AgentLoadDiag& d : AgentRegistry::instance().load_directory(
                 state_.project_dir + "/.wpcode/agent")) {
            std::cout << "[wp_coder] агент: " << d.path << " — " << d.message
                      << std::endl;
        }
    }
    state_.php_bin          = setting_get(cb_, "wp_coder.php_bin", "");
    state_.wp_site_url      = setting_get(cb_, "wp_coder.site_url", "");
    state_.wp_app_user      = setting_get(cb_, "wp_coder.app_user", "");
    state_.wp_app_password  = setting_get(cb_, "wp_coder.app_password", "");
    state_.deploy_proto     = setting_get(cb_, "wp_coder.deploy_proto", "rsync");
    state_.deploy_host      = setting_get(cb_, "wp_coder.deploy_host", "");
    state_.deploy_user      = setting_get(cb_, "wp_coder.deploy_user", "");
    state_.deploy_pass      = setting_get(cb_, "wp_coder.deploy_pass", "");
    state_.deploy_port      = setting_get(cb_, "wp_coder.deploy_port", "");
    state_.deploy_remote_dir = setting_get(cb_, "wp_coder.deploy_remote_dir", "");
    state_.wp_local_url    = setting_get(cb_, "wp_coder.local_url", "");
    state_.agent_system_prompt = setting_get(cb_, "wp_coder.agent_system_prompt", "");
    state_.active_module    = setting_get(cb_, "wp_coder.active_module", "");
    state_.continue_conversation = setting_get(cb_, "wp_coder.continue_conversation", "true") != "false";
    {
        int timeout = 120000;
        std::string t = setting_get(cb_, "wp_coder.llm_timeout_ms", "120000");
        try { timeout = std::stoi(t); } catch (...) {}
        state_.llm_timeout_ms = timeout;
    }
    {
        int steps = 12;
        std::string t = setting_get(cb_, "wp_coder.max_steps", "12");
        try { steps = std::stoi(t); } catch (...) {}
        state_.max_steps = steps > 0 ? steps : 12;
    }
    {
        int budget = 60000;
        std::string t = setting_get(cb_, "wp_coder.session_budget", "60000");
        try { budget = std::stoi(t); } catch (...) {}
        state_.session_budget = budget > 0 ? static_cast<size_t>(budget) : 60000;
    }

    /* И7.2: лимиты модели и настройки компакшна.
     *
     * У хоста их взять нечем — в ABI нет ни окна, ни лимита ответа, — а
     * без них сравнивать нечего, и переполнение не объявляется никогда
     * (compaction::is_overflow). Поэтому это настройки плагина, и
     * значение по умолчанию — «не задано» (0), а не догадка: выдуманное
     * окно выглядело бы в панели как настоящее и молча ломало бы
     * компакшн, выкидывая из истории то, что ещё помещалось.
     *
     * Мусор и отрицательные значения → 0 («не задано»), а не дефолт:
     * стойкое неверное значение лимита хуже его отсутствия, потому что
     * отсутствие видно, а неверное число принимается за правду. */
    {
        const auto read_num = [this](const char* key) -> long long {
            const std::string t = setting_get(cb_, key, "");
            if (t.empty()) return 0;
            try { return std::stoll(t); } catch (...) { return 0; }
        };
        state_.model_limits.context = read_num("wp_coder.context_limit");
        state_.model_limits.input = read_num("wp_coder.input_limit");
        state_.model_limits.max_output = read_num("wp_coder.max_output_tokens");
        if (state_.model_limits.context < 0) state_.model_limits.context = 0;
        if (state_.model_limits.input < 0) state_.model_limits.input = 0;
        if (state_.model_limits.max_output < 0) state_.model_limits.max_output = 0;
        const std::string auto_key = setting_get(cb_, "wp_coder.compaction_auto", "true");
        state_.compaction_config.auto_compact = auto_key != "false";
        const long long reserved = read_num("wp_coder.compaction_reserved");
        state_.compaction_config.reserved = reserved > 0 ? reserved : 0;
        /* tail_turns: 0 = «хвоста нет» (сжимается всё) — поэтому
         * «не задано» здесь -1, а не 0, и разбирать ключ надо ОТДЕЛЬНО:
         * read_num отдаёт 0 и за отсутствие ключа, и за нечисловую мусор,
         * и за честный ноль, а различать их обязательно. Иначе у любого,
         * кто не трогал настройку, дефолтом было бы «сжимать всё» —
         * то есть переполнение приводило бы к потере всей истории, и
         * выглядело бы это как «компакшн не настроен». */
        const auto read_or_unset = [this](const char* key) -> long long {
            const std::string t = setting_get(cb_, key, "");
            if (t.empty()) return -1;
            try { return std::stoll(t); } catch (...) { return -1; }
        };
        const long long tail_turns =
            read_or_unset("wp_coder.compaction_tail_turns");
        state_.compaction_config.tail_turns = tail_turns < 0 ? -1 : tail_turns;
        const long long keep = read_num("wp_coder.compaction_preserve_recent_tokens");
        state_.compaction_config.preserve_recent_tokens = keep > 0 ? keep : 0;
        /* Прореживание вывода инструментов (И7.9). Выключено по умолчанию,
         * как и в порте, и по той же причине: оно меняет то, что видит
         * модель, а не то, что видит пользователь. Включает его тот, кто
         * готов, что модель увидит метки вместо старых выводов и, если
         * понадобится, вызовет инструмент заново. */
        state_.compaction_config.prune =
            setting_get(cb_, "wp_coder.compaction_prune", "false") == "true";
    }

    state_.allowed_external_paths.clear();
    std::string paths_json = setting_get(cb_, "wp_coder.allowed_external_paths", "[]");
    {
        size_t pos = 0;
        while (pos < paths_json.size()) {
            size_t q1 = paths_json.find('"', pos);
            if (q1 == std::string::npos) break;
            size_t q2 = paths_json.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            state_.allowed_external_paths.push_back(paths_json.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
    }

    /* Дефолты разрешений (И2.4). Ставится здесь, а не в init(): только
     * к этому моменту известны доверенные каталоги — data_dir от хоста
     * и каталоги навыков, загруженные плагином. */
    if (!permissions_.has_agent_defaults()) {
        permissions_.set_on_change([this] { invalidate_prompt_cache(); });
        std::vector<std::string> trusted;
        if (cb_.path_data_dir) {
            std::string data = cb_.path_data_dir();
            if (!data.empty()) trusted.push_back(data);
        }
        std::error_code ec;
        std::string tmp = fs::temp_directory_path(ec);
        if (ec) tmp = "/tmp";
        trusted.push_back(tmp);
        for (const auto& d : SkillsManager::instance().source_dirs()) {
            trusted.push_back(d);
        }
        permissions_.apply_agent_defaults(trusted);
    }
    /* Правила пользователя — после дефолтов, чтобы перекрывали их, и
     * вне блока выше: их должно быть видно и при повторном init, когда
     * база уже стоит. */
    permissions_.load_user_rules(
        setting_get(cb_, "wp_coder.permission_rules", ""));

    /* И3.6: доверенные сетевые хосты для curl/wget в bash.
     * Штатно доверен только localhost (задан в конструкторе политики),
     * сюда добавляются адреса сайта из настроек: их плагин и так
     * считает своими и сам по ним ходит при проверке и health check.
     * Доверять произвольный хост по умолчанию нельзя — тогда список
     * разрешённых хостов ничего не разрешает. */
    {
        std::vector<std::string> hosts;
        if (!state_.wp_local_url.empty()) hosts.push_back(state_.wp_local_url);
        if (!state_.wp_site_url.empty()) hosts.push_back(state_.wp_site_url);
        security::trust_command_hosts(hosts);
    }
}

void Engine::save_settings() {
    setting_set(cb_, "wp_coder.project_dir", state_.project_dir);
    setting_set(cb_, "wp_coder.php_bin", state_.php_bin);
    setting_set(cb_, "wp_coder.site_url", state_.wp_site_url);
    setting_set(cb_, "wp_coder.app_user", state_.wp_app_user);
    setting_set(cb_, "wp_coder.app_password", state_.wp_app_password);
    setting_set(cb_, "wp_coder.deploy_proto", state_.deploy_proto);
    setting_set(cb_, "wp_coder.deploy_host", state_.deploy_host);
    setting_set(cb_, "wp_coder.deploy_user", state_.deploy_user);
    setting_set(cb_, "wp_coder.deploy_pass", state_.deploy_pass);
    setting_set(cb_, "wp_coder.deploy_port", state_.deploy_port);
    setting_set(cb_, "wp_coder.deploy_remote_dir", state_.deploy_remote_dir);
    setting_set(cb_, "wp_coder.local_url", state_.wp_local_url);
    setting_set(cb_, "wp_coder.agent_system_prompt", state_.agent_system_prompt);
    setting_set(cb_, "wp_coder.active_module", state_.active_module);
    setting_set(cb_, "wp_coder.continue_conversation", state_.continue_conversation ? "true" : "false");
    setting_set(cb_, "wp_coder.llm_timeout_ms", std::to_string(state_.llm_timeout_ms));
    setting_set(cb_, "wp_coder.max_steps", std::to_string(state_.max_steps));
    setting_set(cb_, "wp_coder.session_budget", std::to_string(state_.session_budget));
}

/* ======================================================================
 * run_task — разбит на компоненты (Фаза D1)
 * goto устранен: cleanup вынесен в lambda.
 * ====================================================================== */

void reset_task_metrics(EngineState& state) {
    /* Метрики задачи обнуляются на каждом запуске, иначе панель показывала
     * бы метрики ПРОШЛОЙ задачи: счётчики, скорость, число шагов.
     *
     * Отдельная функция, а не строки внутри run_task (И6.8): run_task
     * достижим только через worker синглона, и тест на нём зависает на
     * общей очереди предыдущего теста — то есть проверял бы не своё.
     * Здесь правило видно и его можно проверить, не запуская агента.
     *
     * Лок НЕ берётся: вызывающий (run_task) уже держит state_.mtx, а он
     * нерекурсивный и общий с UI — тот же класс, что в permission_outcome.
     *
     * И7.2: сюда же — измерение контекста. Оно относится к ПОСЛЕДНЕМУ
     * запросу, а тот сделан был другой историей; оставить его — значит
     * показать в панели «измерено 40 000 токенов» для пустой истории, а
     * компакшн позже сравнил бы чужое измерение с новой историей. До
     * первого хода задачи работает оценка chars/4. */
    state.llm_total_time = 0;
    state.total_prompt_tokens = 0;
    state.total_completion_tokens = 0;
    state.last_tokens_per_second = 0;
    state.steps = 0;
    state.measured_input_tokens = 0;
}

void Engine::run_task(std::string task) {
    task = text::sanitize_utf8(task);
    auto start_time = std::chrono::steady_clock::now();
    std::string full_response;

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.state = AgentState::Planning;
        state_.last_response.clear();
        state_.response_ready = false;
        state_.abort_requested.store(false);
        /* И6.7: свой токен на ход — см. комментарий в submit(). */
        state_.turn_abort = std::make_shared<AbortToken>();
        /* И6.8: исход сбрасывается на каждый ход, иначе «стоп» прошлой
         * задачи окрашивал бы новую как прерванную. */
        state_.outcome = TaskOutcome::None;
        state_.outcome_reason.clear();
        reset_task_metrics(state_);
        if (state_.session.empty())
            state_.session.push_back(Message::user(task));
        std::cerr << "[wp_coder] run_task: session_msgs=" << state_.session.size()
                  << " continue=" << state_.continue_conversation << std::endl;
    }
    push_event(AgentEvent::Status, "Задача: " + task);

    /* Lambda-очистка — аналог блока done:, вызывается всегда. */
    auto cleanup = [&]() {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);

            /* allow_once: убираем одноразовое разрешение после задачи. */
            if (!state_.once_path.empty()) {
                auto& v = state_.allowed_external_paths;
                auto it = std::find(v.begin(), v.end(), state_.once_path);
                if (it != v.end()) v.erase(it);
                state_.once_path.clear();
            }

            /* И6.8: итоговое состояние выводится из записанного исхода, а
             * не вычисляется здесь заново. Пока исход не задан, задача
             * считается не начатой: иначе ранний выход (LLM не подключён,
             * пустой ход) попадал бы в Done, то есть неудача выглядела бы
             * как успех. */
            state_.state = terminal_state_for(state_.outcome);

            state_.last_response = full_response.empty() ? "(пустой ответ)" : full_response;

            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            state_.last_response_time = duration.count() / 1000.0;

            int comp_tokens = state_.total_completion_tokens;
            double llm_t = state_.llm_total_time;
            if (comp_tokens <= 0 && !state_.last_response.empty())
                comp_tokens = std::max(1, static_cast<int>(state_.last_response.size() / 4));
            state_.last_tokens_generated = comp_tokens;
            state_.last_tokens_per_second = (llm_t > 0.0) ? (double)comp_tokens / llm_t : 0.0;

            state_.response_ready = true;
        }
        state_.response_cv.notify_all();
        /* Resume (5.2): сохраняем диалог после каждой задачи —
         * при следующем запуске можно продолжить. */
        save_session();
    };

    if (!cb_.llm_is_connected || !cb_.llm_is_connected()) {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            /* И6.8: это сбой, а не успех. Раньше здесь был AgentState::Done,
             * и список последних задач показывал выполненную задачу, которой
             * не было. */
            state_.outcome = TaskOutcome::Failed;
            state_.outcome_reason = "LLM не подключён";
        }
        push_event(AgentEvent::Error, "[ошибка] LLM не подключён");
        full_response = "[ошибка] LLM не подключён";
        cleanup();
        return;
    }
    std::cerr << "[wp_coder] run_task: LLM connected, starting plan+loop" << std::endl;

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.last_agent_task = task;
    }

    {
        std::string sys = build_system_prompt();
        std::cerr << "[wp_coder] run_task: system prompt built, len="
                  << sys.size() << std::endl;

        /* D1: Фаза B1 — планирование (если включен). */
        Planner planner(state_, cb_, [this](AgentEvent::Kind k, const std::string& text) {
            push_event(k, text);
        });
        std::cerr << "[wp_coder] run_task: calling planner.plan()..." << std::endl;
        planner.plan(sys);
        std::cerr << "[wp_coder] run_task: planner.plan() done" << std::endl;

        /* FSM: план готов (или пропущен) — переходим к исполнению. */
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.state = AgentState::Executing;
        }

        /* D1: Фаза C — основной ReAct-цикл через AgentLoop. */
        AgentLoop agent_loop(state_, cb_, [this](AgentEvent::Kind k, const std::string& text) {
            push_event(k, text);
        });
        std::cerr << "[wp_coder] run_task: calling agent_loop.run()..." << std::endl;
        const bool loop_ok = agent_loop.run(sys, full_response);
        std::cerr << "[wp_coder] run_task: agent_loop.run() done, ok=" << loop_ok
                  << std::endl;

        /*
         * И6.8: исход выводится ЗДЕСЬ, из того, что вернул цикл.
         *
         * Раньше terminus брался из abort_requested, и любой сбой
         * (провайдер не ответил, пустой ход) попадал в Done — то есть в
         * списке последних задач стояла «выполненная» задача, которой не
         * было, а модель повторяла бы тот же запрос, не понимая, что он
         * не сработал.
         */
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            if (state_.outcome == TaskOutcome::None) {
                if (state_.abort_requested.load()) {
                    state_.outcome = TaskOutcome::Aborted;
                    state_.outcome_reason = "прервано пользователем";
                } else if (loop_ok) {
                    state_.outcome = TaskOutcome::Completed;
                } else {
                    state_.outcome = TaskOutcome::Failed;
                    state_.outcome_reason = "ход агента не состоялся";
                }
            }
        }
    }

    cleanup();
}

/* ======================================================================
 * worker_main
 * ====================================================================== */

void Engine::worker_main() {
    for (;;) {
        std::string task;
        {
            std::unique_lock<std::mutex> lk(state_.mtx);
            state_.cv.wait(lk, [this] {
                return state_.shutting_down || !state_.inbox.empty();
            });
            if (state_.shutting_down) break;
            task = std::move(state_.inbox.front());
            state_.inbox.pop();
        }
        std::cerr << "[wp_coder] worker: picked up task, len=" << task.size() << std::endl;
        run_task(task);
        std::cerr << "[wp_coder] worker: run_task done" << std::endl;
    }
}

void Engine::trim_history_if_needed() {
    /* Снаружи лока НЕ держим: захват state_.mtx — внутри. Именно из-за
     * внешнего лока в AgentLoop раньше был дедлок (D1). */
    std::lock_guard<std::mutex> lk(state_.mtx);
    compress_history(state_.session, state_.session_budget);
}

/* ======================================================================
 * Автосжатие по порогу окна (И7.10)
 * ====================================================================== */

namespace {

/* Снимок того, что нужно решению, и без лока: дальше идёт запрос к
 * модели, а держать state_.mtx на всё время сжатия нельзя (D1). */
struct CompactionSnapshot {
    compaction::ModelLimits limits;
    compaction::CompactionConfig cfg;
    size_t budget = 0;
    long long measured = 0;
    std::vector<Message> history;
};

} // namespace

void Engine::compact_history_if_needed(const compaction::SummaryTurn& turn) {
    CompactionSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        snap.limits = state_.model_limits;
        snap.cfg = state_.compaction_config;
        snap.budget = state_.session_budget;
        snap.measured = state_.measured_input_tokens;
        snap.history = state_.session;
    }
    if (snap.history.empty()) return;

    /* Записать историю обратно. reset_measured обязателен после сводки:
     * измеренные токены относятся к ПРЕЖНЕЙ истории, и без сброса
     * переполнение объявлялось бы снова и снова, то есть агент сжимал бы
     * историю на каждом шаге, платя за запрос к модели. Молчаливое
     * повторение дороже всего: выглядело бы как «сжатие не помогает». */
    const auto commit = [this](std::vector<Message> next, bool reset_measured) {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session = std::move(next);
        if (reset_measured) state_.measured_input_tokens = 0;
    };

    /* 1. Прореживание вывода (И7.9). Дёшево, без запроса к модели, и
     *    часто одного этого хватает: окно переполнено старыми
     *    результатами grep, а не содержанием разговора. */
    bool pruned = false;
    if (snap.cfg.prune) {
        const std::vector<MessagePart*> parts =
            compaction::prune_candidates(snap.history);
        if (!parts.empty()) {
            for (MessagePart* p : parts) p->clear_output();
            pruned = true;
            push_event(AgentEvent::Status,
                "Прореживаю вывод старых инструментов: " +
                 std::to_string(parts.size()) + " вызов.");
        }
    }

    /* 2. Переполнение ли окна. Число — измеренное провайдером, если оно
     *    есть, иначе оценка (И7.2); лимиты неизвестны → переполнения
     *    нет, и сжатие не запускается ни при каких символах. */
    const long long used =
        compaction::context_usage(snap.measured, snap.history).tokens;
    if (!compaction::is_overflow(snap.limits, snap.cfg, used)) {
        /* Окно цело, но локальный бюджет символов никуда не делся: он
         * ограничивает файл сессии, а не модель. Сжатие здесь стоило бы
         * запроса к модели ради того, чего модель не заметит, поэтому
         * работает только аварийная обрезка. */
        if (pruned) commit(std::move(snap.history), false);
        if (model_history_chars(snap.history) > snap.budget) {
            trim_history_if_needed();
        }
        return;
    }

    push_event(AgentEvent::Status,
        "Контекст переполнен (" + std::to_string(used) + " токенов) — "
        "составляю сводку истории.");

    const compaction::Selection sel =
        compaction::select_to_compact(snap.history, snap.limits, snap.cfg);
    const compaction::CompactionResult r =
        compaction::summarize(sel.head, turn);

    if (r.outcome != compaction::CompactionOutcome::Continue) {
        /* Сводка не вышла — пользователю говорится ПОЧЕМУ, иначе обрезка
         * выглядела бы капризом: сообщения исчезают, а человек не знает,
         * что сломалось. */
        push_event(AgentEvent::Status,
            "Сводка не получилась (" + r.reason +
            "). Обрезаю вывод по строкам — работа после этого будет "
            "потеряна частично.");
        if (pruned) commit(std::move(snap.history), false);
        trim_history_if_needed();
        return;
    }

    const size_t was = snap.history.size();
    std::vector<Message> next =
        compaction::compacted_history(sel, r.summary, snap.cfg);
    commit(std::move(next), true);
    save_session();

    push_event(AgentEvent::Status,
        "Сводка готова: " + std::to_string(was - sel.tail.size()) +
        " сообщений свёрнуто, в истории осталось " +
        std::to_string(sel.tail.size() + 2) + ".");

    /* 3. Локальный бюджет символов. Сводка может не уложиться в него
     *    сама по себе (например, сводщик ответил длиннее ожидаемого), и
     *    тогда без аварийной обрезки файл сессии продолжит расти. */
    std::vector<Message> current;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        current = state_.session;
    }
    if (model_history_chars(current) > snap.budget) {
        push_event(AgentEvent::Status,
            "Сводка не поместилась в локальный бюджет — обрезаю по строкам.");
        trim_history_if_needed();
    }
}

/* ======================================================================
 * Тестовый метод trim_session_test (для unit-тестов).
 * ====================================================================== */

void Engine::trim_session_test() {
    std::lock_guard<std::mutex> lk(state_.mtx);
    compress_history(state_.session, state_.session_budget);
}

/* ======================================================================
 * Парсинг вызова инструмента перенесён в core/tool_protocol.{h,cpp} (Фаза D2).
 * В Engine остались тонкие алиасы Engine::extract_action / Engine::parse_action.
 * ====================================================================== */

/* ======================================================================
 * Предложенные правки
 * ====================================================================== */

} // namespace coder
