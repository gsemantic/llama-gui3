// engine.cpp — AI-кодер с разбиением run_task на компоненты (Фаза D1)

#include "agent_components.h"
#include "agent_registry.h"
#include "command_policy.h"
#include "harness_profile.h"
#include "todo_panel.h"
#include "prompts.h"
#include "shell.h"
#include "engine.h"
#include "subagent.h"
#include "session_store.h"
#include "file_lock.h"
#include "limits.h"
#include "json_utils.h"
#include "snapshot.h"

#include <sstream>
#include <vector>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <limits>

namespace coder {

/* Бюджет символов на историю сессии — единый источник: core/limits.h (4.5). */
using limits::kSessionBudget;

namespace fs = std::filesystem;

/* И9.4: env-блок системного промпта (порт opencode session/system.ts:74).
 *
 * Модели полезно знать, где она работает: без этого объяснение «пути
 * относительны» остаётся фразой, а с ним — фактом, который можно
 * проверить. Собирается из того, что плагин действительно знает, и НЕ
 * больше: поле, которого нет, было бы выдумкой.
 *
 * Чего здесь нет и почему:
 *   - МОДЕЛЬ и PROVIDER. Через ABI хост не сообщает ни то, ни другое
 *     (в LlamaHostApi нет ни одного запроса модели), а выдуманная настройка
 *     wp_coder.model стала бы ВТОРЫМ источником истины: человек указал бы
 *     там одно, хот с другим, и env-блок врал бы модели. Поля появятся
 *     вместе с И9.7, который читает профиль harness.
 *   - ЧАСОВОЙ ПОЯС: в хосте его взять неоткуда, а молча ставить системную
 *     зону значило бы печатать время не там, где его читает человек.
 *
 * Дата НЕ инвалидирует кэш промпта: полночь посреди сессии не повод
 * пересобирать промпт ради одной строки, а разница в один день для модели
 * ничего не меняет. */
static std::string build_env_block(const std::string& project_dir) {
    std::string out = "\n\n## ОКРУЖЕНИЕ\n\n";
    if (!project_dir.empty()) {
        /* Working directory и Workspace root folder — одно и то же
         * значение: отдельного «рабочего каталога» у плагина нет, его
         * роль играет wp_coder.project_dir (он же корень worktree и он же
         * cwd). Обе строки оставлены по плану (9.4), и их совпадение —
         * признак того, что второго каталога не появилось. */
        out += "Working directory: " + project_dir + "\n";
        out += "Workspace root folder: " + project_dir + "\n";
        /* .git — каталог у обычного клона и ФАЙЛ у worktree и submodule.
         * Различать обязательно: проверка «каталог существует» сказала бы
         * «нет» в worktree, а это самый вероятный случай у человека,
         * который работает над фича-веткой. */
        std::error_code ec;
        const fs::path git = fs::path(project_dir) / ".git";
        const bool is_repo = fs::exists(git, ec);
        out += std::string("Is directory a git repo: ") +
               (is_repo ? "да" : "нет") + "\n";
    } else {
        out += "Working directory: не задан (wp_coder.project_dir пуст)\n";
        out += "Is directory a git repo: неизвестно\n";
    }
#if defined(__linux__)
    out += "Platform: linux\n";
#elif defined(__APPLE__)
    out += "Platform: macos\n";
#elif defined(_WIN32)
    out += "Platform: windows\n";
#else
    out += "Platform: неизвестно\n";
#endif
    {
        /* Дата — локальная, в формате ISO: модель не должна угадывать
         * формат, а человек читает её в том же календаре, что и модель. */
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_r(&now, &local) != nullptr) {
            char buf[16];
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local) > 0) {
                out += std::string("Date: ") + buf + "\n";
            }
        }
    }
    return out;
}

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
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.shutting_down = true;
        /* И8.14: поставленные в фон задачи снимаются вместе с движком.
         * Молча сбросить очередь нельзя: человек видел, что задачи
         * запущены, и ждал их итогов, — а они исчезли бы без единого
         * слова. Работы ещё не было (дочерняя сессия пишется в конце хода
         * ребёнка), то есть терять нечего, и событие говорит именно это. */
        dropped = state_.background_tasks.size();
        state_.background_tasks.clear();
        state_.permission_cv.notify_all();
        state_.cv.notify_all();
    }
    if (dropped > 0) {
        push_event(AgentEvent::Status,
                   "Движок остановлен: снято фоновых задач — " +
                       std::to_string(dropped) +
                       ". Они ещё не начинали выполняться, работа по ним не"
                       " потеряна.");
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
        /* И8.14: запрос человека обнуляет цепочку автоматических ходов по
         * фоновым результатам. Ответ пользователя всегда разрешает
         * продолжить, и без этого сброса модель, однажды поставившая фоновую
         * задачу на каждом ходу, уже не смогла бы вернуться к обычной работе
         * без вмешательства. */
        state_.background_turns = 0;
        state_.inbox.push(prompt);
        state_.cv.notify_all();
        std::cerr << "[wp_coder] submit: inbox_size=" << state_.inbox.size()
                  << " state=" << agent_state_name(state_.state) << std::endl;
    }
}

void Engine::request_abort() {
    /* ПОРЯДОК ЗДЕСЬ — ЧАСТЬ КАСКАДА, а не оформление (И8.12).
     *
     * Сначала флаг и токен, ПОТОМ освобождение вопросов. Обратный порядок
     * был гонкой, и нашлась она проверкой: освобождённый вопрос отдавал
     * воркеру «пользователь не разрешил», воркер проверял флаг — а он ещё
     * не был выставлен, — и агент продолжал работу после «стоп».
     *
     * Снаружи лока `cancel_all()` — см. комментарий в stop(): PermissionEngine
     * держит свой mtx_ и не берёт state_.mtx, а наоборот брать нельзя
     * (правило 2). */
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
    /* Именно этот вызов вытаскивает worker из ожидания разрешения: иначе
     * «стоп» работал бы не на том шаге, где агент висит на вопросе
     * пользователю. */
    permissions_.cancel_all();
}

snapshot::DiffReport Engine::level_diff(size_t index) {
    /* Копия стека под локом, подсчёт вне. Стек меняет только рабочий
     * поток агента, а читает его ещё и окно, поэтому копия нужна:
     * держать `state_.mtx` на git нельзя (правило 3 SESSION_START). */
    snapshot::UndoStack stack;
    std::string project_dir;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        stack = state_.undo_stack;
        project_dir = state_.project_dir;
    }
    const std::string store = snapshot::store_dir(cb_.path_data_dir ? cb_.path_data_dir()
                                                                   : std::string());
    return stack.diff_step(index, project_dir, store);
}

void Engine::clear_session() {
    /* И10.4: уровни сессии обнуляются вместе с ней. «Очистить сессию» —
     * это кнопка «начать с чистого листа», и держать после неё уровни,
     * к которым можно откатиться, значило бы оставить одну из тех вещей,
     * которые человек только что попросил забыть. */
    std::vector<snapshot::Snapshot> trash;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session.clear();
        state_.recent_calls.clear();
        state_.last_agent_task.clear();
        state_.todos.clear();       /* И4.6: план относится к задаче */
        state_.prompt_dirty = true;
        state_.undo_stack.clear();
        trash = state_.undo_stack.take_trash();
    }  /* mtx отпущен — push_event безопасен */
    /* Копии снимков удаляются ВНЕ лока (правило 3: под state_.mtx не
     * удаляют каталоги — UI ждёт тот же мьютекс). Здесь это делает
     * UI-поток, и это единичная операция по кнопке; в цикле агента то же
     * делает take_step_snapshot на рабочем потоке. */
    snapshot::discard_copies(trash);
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
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        snap = state_.session;
    }
    if (snap.empty()) return;

    /* Идентификатор сессии выдаётся при первом обращении и дальше тот же:
     * новый id означал бы новый файл, то есть потерю resume. Место, где
     * он выдаётся, — ensure_session_id (И8.9), потому что идентификатор
     * нужен и дочерним сессиям субагентов, а не только записи файла. */
    const std::string session_id = ensure_session_id();

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

std::string Engine::ensure_session_id() {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (state_.session_id.empty()) state_.session_id = ids().next_session();
    return state_.session_id;
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

void Engine::ensure_instructions() const {
    /* Настройка берётся ИЗ СОСТОЯНИЯ, а не через cb_.settings_get здесь.
     *
     * Так требует сам файл: настройки читает Engine::load_settings
     * (core/project.h, «Про настройки: ЗДЕСЬ их нет»), и ленивая загрузка
     * не должна стать вторым местом, которое ходит к хосту. Практическое
     * следствие оказалось важнее формального: settings_get — это колбэк,
     * который хост ставит на время init, и тест, вызвавший init с локальной
     * таблицей настроек, оставляет в синглтоне висящий на неё указатель.
     * Пока единственным читателем был load_settings (то есть вызов шёл
     * внутри init, пока таблица жива), это было незаметно; первый же
     * вызов settings_get из сборки промпта — это уже произвольный момент
     * времени, и проверка падала с Segmentation fault вместо строки FAIL. */
    std::string project;
    std::string config_text;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (state_.instructions_loaded) return;
        project = state_.project_dir;
        config_text = state_.instructions_config;
    }

    std::lock_guard<std::mutex> lk(instructions_mtx_);
    /* Пока ждали лок, инструкции могли прочитать и перечитать
     * (reload_instructions сбрасывает флаг) — тогда наш результат был бы
     * устаревшим, и он молча заменил бы свежий. */
    {
        std::lock_guard<std::mutex> slk(state_.mtx);
        if (state_.instructions_loaded) return;
    }

    std::vector<Instruction> loaded = instruction::load(project, config_text);

    {
        std::lock_guard<std::mutex> slk(state_.mtx);
        state_.instructions = std::move(loaded);
        state_.instructions_loaded = true;
    }
    /* Список изменился — кэш промпта, если он был, больше не верен.
     * Флаг снимается здесь, а не в invalidate_prompt_cache(): этот зовётся
     * на каждом todowrite и к загрузке инструкций отношения не имеет. */
    invalidate_prompt_cache();
}

void Engine::reload_instructions() const {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.instructions.clear();
        state_.instructions_loaded = false;
    }
    invalidate_prompt_cache();
}

void Engine::attach_instructions(std::vector<Instruction> more) const {
    if (more.empty()) return;
    size_t added = 0;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        added = instruction::append_new(state_.instructions, std::move(more));
    }
    /* Ничего нового — кэш трогать незачем. Иначе сборка промпта на каждом
     * чтении файла пересобирала бы промпт вхолостую. */
    if (added == 0) return;
    invalidate_prompt_cache();
}

std::string Engine::build_system_prompt() const {
    /* И9.3: порядок блоков — ПОРЯДОК ПЛАНА (session/llm/request.ts:58),
     * и он проверяется тестом, а не держится на память о том, в каком
     * порядке вызваны += :
     *
     *   базовый промпт → env-блок → AGENTS.md → инструкции модулей →
     *   каталог навыков → переопределение текущего запроса.
     *
     * Смысл порядка один: сначала ПРОТОКОЛ, потом СОДЕРЖИМОЕ, а самое
     * специфичное — в конце, где оно перекрывает общее. Поэтому инструкции
     * проекта идут после объяснения протокола (иначе «сначала осмотрись»
     * читалось бы как часть правил репозитория), а режим запроса — в самом
     * конце (он и есть переопределение).
     *
     * env-блока (И9.4) в этой сборке ещё нет: его содержимое — набор фактов
     * о текущем окружении, а заводить пустой заголовок ради порядка значило
     * бы добавить в промпт блок без содержимого.
     *
     * Два блока, которых в списке плана нет, стоят на своих местах и по
     * названной причине: каталог инструментов — между модулем и навыками
     * (он описывает ЧТО доступно, то есть мост между «как» и «о чём»), а
     * план задачи — после каталога, рядом с состоянием сессии.
     */
    /* И9.1: до проверки кэша — источник инструкций может ещё не быть
     * прочитан, и тогда промпт собрался бы без него и запомнился. */
    ensure_instructions();

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
     * Даже при длинном кастомном промпте эта информация не потеряется.
     *
     * И9.3: это ОСОЗНАННОЕ исключение из порядка плана, где корень проекта
     * жил бы в env-блоке сразу за базовым промптом (И9.4). Причина названа
     * в первой строке: назначение этого блока — выжить при длинном
     * пользовательском промпте, а блок после базового при длинном
     * пользовательском промпте окажется в самом конце. Дублировать его в
     * env-блоке не будем: у И9.4 это задача, и там решается, что из
     * «Workspace root folder» остаётся в плане, а что — уже сказано здесь. */
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

    /* И9.4: env-блок. Место — сразу за базовым промптом и ДО правил
     * проекта: это факты о том, где агент работает, а правила проекта
     * читаются в предположении, что факты уже известны. */
    {
        std::string root;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            root = state_.project_dir;
        }
        sys += build_env_block(root);
    }

    /* И9.1: инструкции проекта и пользователя.
     *
     * Место — между промптом агента и инструкциями модуля, и это не
     * вкусовое решение, а следствие порядка из плана (9.3): правила
     * проекта идут после базового промпта, но ДО инструкций модуля,
     * потому что модуль описывает работу с доменом, а проект — как
     * именно её делать в этом репозитории.
     *
     * Один текст на все источники не годится: потерялось бы, какой файл
     * что сказал, а при конфликте (проект против модуля) не нашлось бы,
     * что важнее. Блоки собирает instruction::render — по одному на
     * источник, с заголовком, названным в kBaseSystemPrompt. */
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        sys += instruction::render(state_.instructions);
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

/* И11.7: клик по чекбобксу плана. Зовётся с UI-потока. */
bool Engine::toggle_todo(const std::string& id) {
    bool changed = false;
    {
        /* Лок берётся на время ТОЛЬКО правки плана и отпускается до
         * invalidate_prompt_cache(): тот сам ничего под локом не берёт,
         * а держать `state_.mtx` лишний раз нельзя — его ждёт окно. */
        std::lock_guard<std::mutex> lk(state_.mtx);
        changed = todo_panel::apply_toggle(state_.todos, id);
    }
    /* Сброс кэша ВНЕ лока и только при реальном изменении: план печатается
     * в системном промпте, и без этого модель видела бы старый список до
     * конца сессии. Клик по несуществующему пункту (план успел измениться
     * между кадром и кликом) кэша не касается — менять нечего. */
    if (changed) invalidate_prompt_cache();
    return changed;
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
    /* И9.1: список источников инструкций из настройки. Читается здесь,
     * вместе со всеми остальными настройками, и по той же причине: это
     * единственное место, где плагин ходит к хосту за настройками.
     * Список разбирается при первой сборке промпта (ensure_instructions),
     * потому что там известен корень проекта, а здесь — нет. */
    state_.instructions_config = setting_get(cb_, "wp_coder.instructions", "");
    /* Перечитывание настроек обязано сбрасывать и ПРОЧИТАННЫЕ инструкции:
     * сменился корень проекта или список источников, а правила прежнего
     * проекта остались бы в промпте. Само значение читается здесь, а
     * разбирается при первой сборке промпта — там известен корень. */
    reload_instructions();
    state_.active_module    = setting_get(cb_, "wp_coder.active_module", "");
    state_.continue_conversation = setting_get(cb_, "wp_coder.continue_conversation", "true") != "false";
    {
        /* Таймаут: сначала настройка, потом профиль (И9.7).
         *
         * Порядок — «явное важнее профиля», и он читается в одну строку:
         * пустая настройка означает «человек не задавал», и тогда берётся
         * профиль. Обратный порядок (профиль поверх настройки) сделал бы
         * настройку бесполезной: человек написал бы 60000, а получил бы
         * 300000 из debug_verbose и не понял бы почему.
         *
         * Мусор и НОЛЬ → дефолт 120000, и это ОТДЕЛЬНОЕ решение, а не
         * побочный эффект: раньше неразбираемое значение давало 0, то
         * есть deadline в прошлом и мгновенный таймаут (host_bridge/
         * llm_blocking.cpp:93) — опечатка в настройке обрывала бы работу
         * агента на первом же запросе. Значит читаемое «0» из профиля
         * отвергается (см. parse_profile), а «0» в настройке приводится к
         * дефолту: ноль таймаута не выражает намерения человека, его
         * выражает опечатка.
         *
         * Читать профиль здесь нельзя: к этому месту ещё не прочитаны
         * правила разрешений, а профиль кладёт запреты в них (см.
         * apply_session_profile ниже). */
        const std::string t = setting_get(cb_, "wp_coder.llm_timeout_ms", "");
        state_.llm_timeout_setting = t;   // решает исход vs профиль, И9.7
        int timeout = 0;
        if (!t.empty()) {
            try { timeout = std::stoi(t); } catch (...) {}
        }
        state_.llm_timeout_ms = timeout;
    }
    /* И9.7: профиль harness. Каталог — настройка, а если она пуста, то
     * поставленный каталог плагина (тот же приём, что у навыков). */
    {
        std::string dir = setting_get(cb_, "wp_coder.profiles_dir", "");
        if (dir.empty()) dir = state_.profiles_bundled_dir;
        state_.profiles_dir = dir;
        /* Имя профиля — с приведением регистра, как имена агентов (И8.1):
         * его пишет человек, и «Secure_Audit» в настройке означал бы
         * «профиль не найден» при файле secure_audit.json. */
        std::string name = setting_get(cb_, "wp_coder.profile", "");
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        state_.profile_name = name;
    }
    {
        int steps = 12;
        std::string t = setting_get(cb_, "wp_coder.max_steps", "12");
        try { steps = std::stoi(t); } catch (...) {}
        state_.max_steps = steps > 0 ? steps : 12;
    }
    /* И8.8: предел вложенности субагентов. Рядом с max_steps, потому что
     * это тоже предел хода, а не отдельная подсистема.
     *
     * Мусор и отрицательные значения → дефолт, и это НЕ «предела нет»:
     * настройку, которую не удалось разобрать, прочитать как «вкладывайся
     * сколько хочешь» — значит вернуть ровно то, ради чего предел есть
     * (И8.7, отклонение №78). Обратное тоже верно: 0 — не мусор, а
     * запрет делегирования вообще, и подменять его единицей значило бы
     * сделать «субагентов нет» неотличимым от опечатки.
     *
     * Разбор строгий, как в agent_registry (И8.2): std::stoll берёт
     * ПРЕФИКС, и из «2abc» вышел бы 2 — то есть опечатка стала бы
     * настройкой. Число вне int — тоже не настройка. */
    {
        const std::string t = setting_get(cb_, "wp_coder.subagent_depth", "");
        int depth = limits::kSubagentDepthLimit;
        try {
            size_t used = 0;
            const long long v = std::stoll(t, &used);
            if (used == t.size() && v >= 0 &&
                v <= static_cast<long long>(std::numeric_limits<int>::max())) {
                depth = static_cast<int>(v);
            }
        } catch (...) {}
        state_.subagent_depth = depth;
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

    /* И9.7: профиль harness. После правил пользователя — запреты профиля
     * кладутся в конец набора. */
    apply_session_profile();

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

/* И9.7: профиль harness → сессия. Разбор, таймаут, команды, запреты.
 *
 * Что делает и что НЕ делает — по одному правилу: поле применяется, если
 * у плагина есть место, где его применение имеет смысл. Отсюда и список
 * в core/harness_profile.h: temperature/max_tokens ждут И11.14 (D23),
 * model/allowed_extensions/rag_enabled применять нечем.
 *
 * ПОЧЕМУ ЗДЕСЬ, А НЕ В ИНИЦИАЛИЗАТОРЕ ПРОФИЛЯ. Инициализация читала бы
 * файл и молчала; здесь у профиля есть место для ПРИЧИНЫ (profile_error),
 * а профиль, который не применился, обязан быть виден — иначе человек
 * написал бы «secure_audit» и ходил бы с правами обычной сессии, ничего
 * об этом не подозревая.
 *
 * ПОВТОРНЫЙ ВЫЗОВ ИДЕМПОТЕНТЕН. Именно поэтому у политики команд есть
 * reset_allowed_binaries(), а запреты разрешений кладутся по сигнатуре
 * (список ключей): load_settings зовётся на каждом init, и без сброса
 * смена профиля оставила бы границу прежнего. */
void Engine::apply_session_profile() {
    std::string name;
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        name = state_.profile_name;
        dir = state_.profiles_dir;
    }

    /* Даже без профиля границу команды надо вернуть к дефолту: иначе
     * снятый профиль (или смена на другой) оставил бы прежнее сужение. */
    command_policy().reset_allowed_binaries();

    if (name.empty()) {
        /* Прежние запреты профиля снимаются даже когда профиля больше
         * нет: человек убрал настройку, и его «всегда» из ответа на
         * вопрос не должен упираться в запрет, которого он уже отменил. */
        permissions_.apply_profile_rules("", "", {});
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.has_profile = false;
        state_.profile = harness::Profile();
        state_.profile_error.clear();
        /* Таймаут: настройки не было, профиля нет — дефолт плагина. */
        if (state_.llm_timeout_ms <= 0) state_.llm_timeout_ms = 120000;
        return;
    }

    harness::Profile profile;
    std::string why;
    if (!harness::load_profile(dir, name, &profile, &why)) {
        /* Профиль не применился — значит, прежние его запреты тоже не
         * должны остаться в силе: иначе сессия была бы ограничена
         * настройкой, которой человек больше не задавал. */
        permissions_.apply_profile_rules("", "", {});
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.has_profile = false;
        state_.profile = harness::Profile();
        state_.profile_error = why;
        if (state_.llm_timeout_ms <= 0) state_.llm_timeout_ms = 120000;
        std::cout << "[wp_coder] профиль harness: " << why << std::endl;
        return;
    }

    /* Таймаут профиля — только если настройка не задана (см. load_settings):
     * явная настройка человека важнее значения из файла. */
    std::string cmd_why;
    std::string keys_why;
    const std::vector<std::string> denied = harness::denied_keys(
        profile.tools_policy, ToolsRegistry::instance().defs(), &keys_why);
    if (!keys_why.empty()) {
        /* tools_policy пустой в файле — это не ошибка разбора: поле
         * необязательное, и профиль без него просто ничего не сужает.
         * Неизвестное значение parse_profile уже отверг. */
        std::cout << "[wp_coder] профиль harness " << profile.name << ": "
                  << keys_why << std::endl;
    }
    if (!harness::apply_allowed_commands(command_policy(),
                                         profile.allowed_commands, &cmd_why)) {
        std::cout << "[wp_coder] профиль harness " << profile.name
                  << ": команды не применены — " << cmd_why << std::endl;
    }

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.has_profile = true;
        state_.profile = profile;
        state_.profile_error.clear();
        /* Таймаут профиля — только если настройка не задана. Проверка идёт
         * по СЫРОЙ строке настройки, а не по разобранному числу: «0» и мусор
         * — это тоже «человек написал», и молча заменить их значением
         * профиля значило бы стереть его опечатку вместо того, чтобы её
         * показать. Мусор и ноль разбираются в дефолт в load_settings. */
        if (profile.has_timeout_ms && state_.llm_timeout_setting.empty()) {
            state_.llm_timeout_ms = profile.timeout_ms;
        }
        if (state_.llm_timeout_ms <= 0) state_.llm_timeout_ms = 120000;
    }

    /* Запреты — в конец набора правил, поэтому перекрывают правила
     * пользователя (last match wins, core/permission.h). Человеку при этом
     * остаётся последнее слово: его ответ «всегда» кладётся правилом
     * ПОЗЖЕ (PermissionEngine::approve) и потому побеждает. Профиль —
     * это поза по умолчанию, а не запрет на запрет.
     *
     * apply_profile_rules, а не add_rule в цикле: он ПЕРЕСБИРАЕТ свои
     * правила вместо добавления, иначе повторный init копил бы копии, а
     * смена профиля не отпустила бы то, что сужал прежний. */
    permissions_.apply_profile_rules(profile.name, profile.tools_policy, denied);
    if (!denied.empty()) {
        std::cout << "[wp_coder] профиль harness " << profile.name
                  << ": запрещено " << denied.size() << " ключей инструментов ("
                  << profile.tools_policy << ")" << std::endl;
    }
    for (const std::string& k : profile.unknown_keys) {
        std::cout << "[wp_coder] профиль harness " << profile.name
                  << ": поле «" << k << "» плагину неизвестно" << std::endl;
    }

    /* Набор видимых инструментов мог измениться. */
    invalidate_prompt_cache();
}

void Engine::set_profiles_bundled_dir(std::string dir) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.profiles_bundled_dir = std::move(dir);
}

std::shared_ptr<const harness::Profile> Engine::profile_for_agent(
    const std::string& agent_profile_name, std::string* why) const {
    if (why) why->clear();
    if (agent_profile_name.empty()) return nullptr;
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        dir = state_.profiles_dir;
    }
    harness::Profile p;
    std::string reason;
    if (!harness::load_profile(dir, agent_profile_name, &p, &reason)) {
        if (why) *why = reason;
        return nullptr;
    }
    return std::make_shared<const harness::Profile>(std::move(p));
}

bool Engine::session_profile(harness::Profile* out, std::string* error) const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (out) *out = state_.profile;
    if (error) *error = state_.profile_error;
    return state_.has_profile;
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
        /* И8.14: ход закончился — выполняем то, что он поставил в фон, и
         * доставляем результаты. Здесь, а не в потоке фоновой задачи:
         * область выполнения одна на весь движок, и второй поток отдал бы
         * родителю enforcement по правилам ребёнка (обоснование — в
         * core/subagent.h). */
        deliver_background_tasks();
    }
}

/* И8.14: разбор очереди фоновых задач и доставка результатов.
 *
 * Результат доставить — значит начать ход: родительский ход к этому
 * моменту закончился, а результат лежит в сессии сообщением, которое
 * без хода никто не прочитает. Такой ход стоит денег, и его никто не
 * заказывал, поэтому цепочка ограничена (limits::kBackgroundTurnLimit), а
 * сбрасывает её любой запрос человека (submit). */
void Engine::deliver_background_tasks() {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (state_.shutting_down) return;
    }
    /* Возвращает false, если задач не было или их остановил пользователь
     * (см. run_background_tasks) — оба случая означают, что читать в
     * ходе нечего. */
    if (!run_background_tasks(*this)) return;

    bool over_limit = false;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        over_limit = state_.background_turns >= limits::kBackgroundTurnLimit;
        if (!over_limit) ++state_.background_turns;
    }
    if (over_limit) {
        push_event(AgentEvent::Status,
                   "Результаты фоновых задач в диалоге, но автоматический ход "
                   "не начат: предел цепочки — " +
                       std::to_string(limits::kBackgroundTurnLimit) +
                       ". Скажите, что с ними делать.");
        return;
    }
    /* Текст задачи — короткая подпись для события и last_agent_task: сами
     * результаты лежат в сессии отдельным сообщением, и повторять их в
     * подписи значило бы показать пользователю дважды. */
    run_task("Фоновые задачи завершены — их результаты в диалоге. "
             "Разберись с ними и продолжи задачу.");
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
