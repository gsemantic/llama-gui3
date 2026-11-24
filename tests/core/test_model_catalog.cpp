#include "../../include/core/model_catalog.h"
#include "../test_framework.h"

#include <string>

using namespace llama_gui::core;

/* endpoint нормализуется так, чтобы конфиг хоста и base_url каталога
 * сравнивались одинаково: хвостовой `/` и `/chat/completions` — не часть
 * провайдера. */
void test_normalize_endpoint() {
    TEST_ASSERT_EQUAL(normalize_endpoint("https://OpenRouter.ai/api/v1/"),
                      std::string("https://openrouter.ai/api/v1"));
    TEST_ASSERT_EQUAL(normalize_endpoint("https://openrouter.ai/api/v1/chat/completions"),
                      std::string("https://openrouter.ai/api/v1"));
    TEST_ASSERT_EQUAL(normalize_endpoint("  HTTP://127.0.0.1:1234/v1  "),
                      std::string("http://127.0.0.1:1234/v1"));
}

void test_normalize_model_name() {
    TEST_ASSERT_EQUAL(normalize_model_name("Llama 3.1 8B Instruct"),
                      std::string("llama-3-1-8b-instruct"));
    TEST_ASSERT_EQUAL(normalize_model_name("  Qwen/Qwen3-32B  "),
                      std::string("qwen-qwen3-32b"));
}

static const char* kCatalog = R"JSON({
  "alibaba": {
    "id": "alibaba",
    "name": "Alibaba",
    "models": {
      "qwen3-32b": {
        "id": "qwen3-32b",
        "canonical_id": "qwen/qwen3-32b",
        "name": "Qwen3 32B",
        "family": "qwen",
        "limit": {"context": 131072, "output": 8192},
        "capabilities": {"tools": {"supported": true}, "reasoning": {"supported": false}},
        "cost": {"text": {"input": {"price": 0.5, "cache": {"read": 0.1, "write": 0.2}},
                          "output": {"price": 1.5}}},
        "api": {"chat-completions": {"base_url": "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"}}
      },
      "free-zero": {
        "id": "free-zero",
        "canonical_id": "qwen/free-zero",
        "name": "Free Zero",
        "family": "qwen",
        "limit": {"context": 4096, "output": 1024},
        "capabilities": {"tools": {"supported": false}},
        "cost": {"text": {"input": {"price": 0}, "output": {"price": 0}}},
        "api": {"chat-completions": {"base_url": "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"}}
      },
      "no-tariff": {
        "id": "no-tariff",
        "canonical_id": "qwen/no-tariff",
        "name": "No Tariff",
        "family": "qwen",
        "limit": {"context": 4096, "output": 1024},
        "capabilities": {"tools": {"supported": false}},
        "api": {"chat-completions": {"base_url": "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"}}
      },
      "tools-unknown": {
        "id": "tools-unknown",
        "name": "Tools Unknown",
        "limit": {"context": 2048, "output": 512},
        "api": {"chat-completions": {"base_url": "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"}}
      }
    }
  },
  "openrouter": {
    "id": "openrouter",
    "name": "OpenRouter",
    "models": {
      "qwen/qwen3-32b": {
        "id": "qwen/qwen3-32b",
        "canonical_id": "qwen/qwen3-32b",
        "name": "Qwen3 32B (OR)",
        "family": "qwen",
        "limit": {"context": 131072, "output": 8192},
        "capabilities": {"tools": {"supported": true}, "reasoning": {"supported": true}},
        "cost": {"text": {"input": {"price": 0.6}, "output": {"price": 1.8}}},
        "api": {"chat-completions": {"base_url": "https://openrouter.ai/api/v1"}}
      }
    }
  }
})JSON";

void test_parse_projection() {
    const auto models = parse_catalog(kCatalog);
    TEST_ASSERT_EQUAL(models.size(), static_cast<size_t>(5));

    const CatalogModel* found = nullptr;
    const CatalogModel* free_zero = nullptr;
    const CatalogModel* no_tariff = nullptr;
    const CatalogModel* tools_unknown = nullptr;
    for (const auto& m : models) {
        if (m.provider == "alibaba" && m.model_id == "qwen3-32b") found = &m;
        if (m.provider == "alibaba" && m.model_id == "free-zero") free_zero = &m;
        if (m.provider == "alibaba" && m.model_id == "no-tariff") no_tariff = &m;
        if (m.provider == "alibaba" && m.model_id == "tools-unknown") tools_unknown = &m;
    }

    TEST_ASSERT(found != nullptr);
    TEST_ASSERT(free_zero != nullptr);
    TEST_ASSERT(no_tariff != nullptr);
    TEST_ASSERT(tools_unknown != nullptr);
    /* tools_known отличает «каталог сказал: не умеет» от «каталог молчит». */
    TEST_ASSERT(found->tools_known && found->tools_supported);
    TEST_ASSERT(no_tariff->tools_known && !no_tariff->tools_supported);
    TEST_ASSERT(!tools_unknown->tools_known);
    /* Бесплатная модель: тариф есть, цены нулевые. */
    TEST_ASSERT(free_zero->has_cost);
    TEST_ASSERT_EQUAL(free_zero->price_in, 0.0);
    TEST_ASSERT_EQUAL(free_zero->price_out, 0.0);
    /* Записи без тарифа: не путать с бесплатной. */
    TEST_ASSERT(!no_tariff->has_cost);
    TEST_ASSERT_EQUAL(found->family, std::string("qwen"));
    TEST_ASSERT_EQUAL(found->context, 131072);
    TEST_ASSERT_EQUAL(found->max_output, 8192);
    TEST_ASSERT(found->tools_supported);
    TEST_ASSERT(!found->reasoning);
    TEST_ASSERT(found->has_cost);
    TEST_ASSERT_EQUAL(found->price_in, 0.5);
    TEST_ASSERT_EQUAL(found->price_out, 1.5);
    TEST_ASSERT_EQUAL(found->cache_read, 0.1);
    TEST_ASSERT_EQUAL(found->cache_write, 0.2);
    TEST_ASSERT_EQUAL(found->base_url,
                      std::string("https://dashscope-intl.aliyuncs.com/compatible-mode/v1"));
}

/* Точное совпадение по endpoint важнее: у хоста «Qwen (DashScope)», а в
 * каталоге провайдер `alibaba` — имя не ключ, ключ — base_url. */
void test_match_exact_by_endpoint() {
    const auto models = parse_catalog(kCatalog);
    const auto info = match_model(
        models, "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "qwen3-32b");
    TEST_ASSERT(info.found);
    TEST_ASSERT_EQUAL(info.source, std::string("exact"));
    TEST_ASSERT_EQUAL(info.provider, std::string("alibaba"));
    TEST_ASSERT(!info.reasoning);
}

/* Тот же model_id у другого провайдера — только справочно. */
void test_match_reference_when_endpoint_differs() {
    const auto models = parse_catalog(kCatalog);
    const auto info = match_model(models, "https://example.invalid/v1", "qwen3-32b");
    TEST_ASSERT(info.found);
    TEST_ASSERT_EQUAL(info.source, std::string("reference"));
}

void test_match_unknown() {
    const auto models = parse_catalog(kCatalog);
    const auto info = match_model(models, "https://openrouter.ai/api/v1", "no-such-model");
    TEST_ASSERT(!info.found);
    TEST_ASSERT_EQUAL(info.source, std::string("unknown"));
}

/* Локальная модель: source всегда local, подсказки по имени — если есть. */
void test_match_local_by_name() {
    const auto models = parse_catalog(kCatalog);
    const auto info = match_local_model(models, "/models/qwen3-32b.Q4_K_M.gguf");
    TEST_ASSERT_EQUAL(info.source, std::string("local"));
    TEST_ASSERT(info.found);
}

void test_model_info_json_shape() {
    const auto models = parse_catalog(kCatalog);
    const auto info = match_model(models, "", "qwen/qwen3-32b");
    const auto j = info.to_json();
    TEST_ASSERT(j.find("\"ok\":1") != std::string::npos);
    TEST_ASSERT(j.find("\"source\":\"reference\"") != std::string::npos);
    TEST_ASSERT(j.find("\"context\":131072") != std::string::npos);
    TEST_ASSERT(j.find("\"has_cost\":true") != std::string::npos);
}

/* Окно модели: профиль провайдера и локальный n_ctx важнее справочного
 * совпадения по имени, но точное совпадение endpoint + model_id — важнее
 * профиля. Иначе `glm-4.7-flash` у чужого провайдера подставлял бы окно
 * первого попавшегося варианта (131072 вместо 200000). */
void test_resolve_context_window_prefers_exact_then_profile() {
    const auto models = parse_catalog(kCatalog);
    const CatalogModelInfo exact = match_model(
        models, "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
        "qwen3-32b");
    TEST_ASSERT_EQUAL(exact.source, std::string("exact"));
    /* Точное совпадение: каталог (131072) важнее профиля (999999). */
    TEST_ASSERT_EQUAL(resolve_context_window(exact, 999999, 0), 131072);

    const CatalogModelInfo reference = match_model(
        models, "https://example.invalid/v1", "qwen3-32b");
    TEST_ASSERT_EQUAL(reference.source, std::string("reference"));
    /* Совпадение только по имени: профиль провайдера точнее. */
    TEST_ASSERT_EQUAL(resolve_context_window(reference, 200000, 0), 200000);
    /* Профиля нет: справочный каталог всё же лучше «неизвестно». */
    TEST_ASSERT_EQUAL(resolve_context_window(reference, 0, 0), 131072);
}

void test_resolve_context_window_local_uses_loaded_n_ctx() {
    const auto models = parse_catalog(kCatalog);
    const CatalogModelInfo local = match_local_model(
        models, "/models/qwen3-32b.Q4_K_M.gguf");
    TEST_ASSERT_EQUAL(local.source, std::string("local"));
    /* Каталог знает модель: показываем advertised окно модели (131072),
     * а не фактически загруженный n_ctx=4096. */
    TEST_ASSERT_EQUAL(resolve_context_window(local, 0, 4096), 131072);
    /* Каталог молчит: n_ctx — это лучше, чем «неизвестно». */
    CatalogModelInfo unknown_local;
    unknown_local.source = "local";
    TEST_ASSERT_EQUAL(resolve_context_window(unknown_local, 0, 4096), 4096);
    /* Ничего не известно — 0, а не выдуманное окно. */
    const CatalogModelInfo unknown;
    TEST_ASSERT_EQUAL(resolve_context_window(unknown, 0, 0), 0);
}

int main() {
    REGISTER_TEST(test_normalize_endpoint);
    REGISTER_TEST(test_normalize_model_name);
    REGISTER_TEST(test_parse_projection);
    REGISTER_TEST(test_match_exact_by_endpoint);
    REGISTER_TEST(test_match_reference_when_endpoint_differs);
    REGISTER_TEST(test_match_unknown);
    REGISTER_TEST(test_match_local_by_name);
    REGISTER_TEST(test_model_info_json_shape);
    REGISTER_TEST(test_resolve_context_window_prefers_exact_then_profile);
    REGISTER_TEST(test_resolve_context_window_local_uses_loaded_n_ctx);
    return test::TestRunner::instance().run();
}
