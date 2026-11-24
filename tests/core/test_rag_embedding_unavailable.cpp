// test_rag_embedding_unavailable.cpp — проверка контракта R0.1:
//
// Когда источник эмбеддингов не настроен (нет пути к модели и нет
// серверной URL), RagManager обязан:
//   - возвращать ПУСТОЙ вектор из generate_embedding (не нулевой, не
//     случайный, не n-gram hash);
//   - не писать чанки с пустыми эмбеддингами в индекс чат-истории;
//   - не падать при build_rag_prompt с пустыми chunks (раньше там было
//     деление на ноль);
//   - search_* возвращали пустой результат без «почти совпадений».
//
// Пустой вектор — однозначный сигнал вызывающему: «эмбеддингов нет».
// Нулевой вектор или случайный — правдоподобный мусор, неотличимый
// от успеха, и потому опаснее явного отказа.
//
// ИЗОЛЯЦИЯ ОТ МАШИНЫ. RagManager в конструкторе поднимает менеджер
// профилей, и без явного HOME он читает ~/.llama-gui/rag_profiles/
// пользователя: на машине разработчика там реальный индекс на тысячи
// чанков, и тест «прошёл бы» по чужому индексу, ничего не проверив.
// Поэтому HOME подменяется на пустой временный каталог ДО создания
// RagManager, а после — возвращается обратно. Это не идеально (глобальная
// переменная окружения), но RagManager не даёт переинициализировать
// профиль после构造; настоящая инъекция каталога — задача R1.

#include "../../include/core/rag_manager.h"
#include "../test_framework.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace llama_gui::core;

namespace {

// Временный HOME на время жизни менеджера. Возвращает прежнее значение
// через restore.
class ScopedHome {
public:
    explicit ScopedHome(const std::string& dir) : saved_(getenv("HOME")) {
        setenv("HOME", dir.c_str(), 1);
    }
    ~ScopedHome() {
        if (saved_) setenv("HOME", saved_, 1);
        else unsetenv("HOME");
    }
    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

private:
    const char* saved_;
};

// Каталог, который удалится сам. Создаётся до подмены HOME.
std::filesystem::path make_temp_home() {
    std::filesystem::path p = std::filesystem::temp_directory_path() /
        ("llama_gui_rag_test_" + std::to_string(reinterpret_cast<uintptr_t>(&p)));
    std::filesystem::create_directories(p);
    return p;
}

} // namespace

// Создаём RagManager с пустым путём и пустым HOME: load_model() вернёт
// false, embedding_generator_ будет сброшен, профилей и индексов нет.
static RagManager* make_unavailable() {
    static RagManager* rag = nullptr;
    static std::filesystem::path tmp = make_temp_home();
    static ScopedHome home(tmp.string());
    if (!rag) {
        rag = new RagManager("");  // пустой путь = нет модели
    }
    return rag;
}

// ============================================================================
// generate_embedding
// ============================================================================

void test_generate_embedding_returns_empty_when_unavailable() {
    auto* rag = make_unavailable();
    auto vec = rag->generate_embedding("hello world");
    TEST_ASSERT_TRUE(vec.empty());
}

void test_generate_embedding_empty_text_returns_empty() {
    auto* rag = make_unavailable();
    auto vec = rag->generate_embedding("");
    TEST_ASSERT_TRUE(vec.empty());
}

void test_generate_embedding_repeated_calls_still_empty() {
    // Проверяем, что поведение стабильное: первый вызов и сотый дают
    // одинаково пустой результат, а не «сначала пусто, потом что-то».
    auto* rag = make_unavailable();
    for (int i = 0; i < 100; ++i) {
        auto vec = rag->generate_embedding("repeated query " + std::to_string(i));
        TEST_ASSERT_TRUE(vec.empty());
    }
}

// ============================================================================
// search_external_documents
// ============================================================================

void test_search_external_documents_unavailable_returns_empty() {
    auto* rag = make_unavailable();
    auto results = rag->search_external_documents("test query", 5);
    TEST_ASSERT_EQUAL(results.size(), 0u);
}

void test_search_external_documents_empty_query_returns_empty() {
    auto* rag = make_unavailable();
    auto results = rag->search_external_documents("", 5);
    TEST_ASSERT_EQUAL(results.size(), 0u);
}

// ============================================================================
// cache_query_response
// ============================================================================

void test_cache_query_response_unavailable_does_not_write_chunks() {
    auto* rag = make_unavailable();
    const size_t before = rag->get_chat_history_chunks_count();
    rag->cache_query_response("what is X?", "X is something");
    const size_t after = rag->get_chat_history_chunks_count();
    // Индекс не должен вырасти: без эмбеддингов чанк бесполезен, и его
    // presence создавал бы иллюзию работающего кэша.
    TEST_ASSERT_EQUAL(before, after);
}

void test_cache_query_response_unavailable_multiple_calls_no_growth() {
    auto* rag = make_unavailable();
    const size_t before = rag->get_chat_history_chunks_count();
    for (int i = 0; i < 5; ++i) {
        rag->cache_query_response("q" + std::to_string(i),
                                  "a" + std::to_string(i));
    }
    const size_t after = rag->get_chat_history_chunks_count();
    TEST_ASSERT_EQUAL(before, after);
}

// ============================================================================
// build_rag_prompt
// ============================================================================

void test_build_rag_prompt_empty_chunks_does_not_crash() {
    // Попутный фикс R0.1: раньше при пустых chunks было деление на ноль
    // в строке `max_context_chars / chunks_to_use`.
    auto* rag = make_unavailable();
    std::vector<RagChunk> empty;
    std::string prompt = rag->build_rag_prompt("what is the answer?", empty, false);
    // Промпт должен содержать вопрос
    TEST_ASSERT_TRUE(prompt.find("what is the answer?") != std::string::npos);
    // И НЕ должен содержать маркеров несуществующих чанков («[1]»)
    TEST_ASSERT_TRUE(prompt.find("[1]") == std::string::npos);
}

void test_build_rag_prompt_empty_chunks_cloud_does_not_crash() {
    // Та же проверка для ветки облачной модели (другие лимиты)
    auto* rag = make_unavailable();
    std::vector<RagChunk> empty;
    std::string prompt = rag->build_rag_prompt("cloud question", empty, true);
    TEST_ASSERT_TRUE(prompt.find("cloud question") != std::string::npos);
}

// ============================================================================
// find_cached_response
// ============================================================================

void test_find_cached_response_unavailable_returns_empty() {
    auto* rag = make_unavailable();
    // При недоступных эмбеддингах кэш не должен «почти совпадать» ни с
    // чем — пустой вектор запроса даёт пустой результат поиска.
    std::string cached = rag->find_cached_response("anything");
    TEST_ASSERT_TRUE(cached.empty());
}

int main() {
    REGISTER_TEST(test_generate_embedding_returns_empty_when_unavailable);
    REGISTER_TEST(test_generate_embedding_empty_text_returns_empty);
    REGISTER_TEST(test_generate_embedding_repeated_calls_still_empty);

    REGISTER_TEST(test_search_external_documents_unavailable_returns_empty);
    REGISTER_TEST(test_search_external_documents_empty_query_returns_empty);

    REGISTER_TEST(test_cache_query_response_unavailable_does_not_write_chunks);
    REGISTER_TEST(test_cache_query_response_unavailable_multiple_calls_no_growth);

    REGISTER_TEST(test_build_rag_prompt_empty_chunks_does_not_crash);
    REGISTER_TEST(test_build_rag_prompt_empty_chunks_cloud_does_not_crash);

    REGISTER_TEST(test_find_cached_response_unavailable_returns_empty);

    return test::TestRunner::instance().run();
}
