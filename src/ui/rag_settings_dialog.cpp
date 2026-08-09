#include "../include/ui/rag_settings_dialog.h"
#include "imgui.h"
#include <cstring>
#include <iostream>
#include "../include/core/settings.h"
#include "../include/ui/main_window.h"
#include "../include/ui/localization_manager.h"

namespace llama_gui {
namespace ui {

RagSettingsDialog::RagSettingsDialog(void* settings, void* main_window)
    : settings_ptr_(settings)
    , main_window_ptr_(main_window)
{
    // Приводим void* к Settings* и инициализируем переменные
    llama_gui::core::Settings* settings_obj = static_cast<llama_gui::core::Settings*>(settings);

    max_chunks_in_memory_ = settings_obj->rag().max_chunks_in_memory;
    similarity_threshold_ = settings_obj->rag().similarity_threshold;
    max_embedding_cache_size_ = settings_obj->rag().max_embedding_cache_size;
    max_tokens_per_chunk_ = settings_obj->rag().max_tokens_per_chunk;
    search_k_ = settings_obj->rag().search_k;
    mmr_lambda_ = settings_obj->rag().mmr_lambda;
    enable_mmr_ = settings_obj->rag().enable_mmr;
    enable_caching_ = settings_obj->rag().enable_caching;
    rag_mode_ = static_cast<int>(settings_obj->rag().rag_mode);

    // Параметры гибридного поиска
    enable_hybrid_search_ = settings_obj->rag().enable_hybrid_search;
    keyword_boost_weight_ = settings_obj->rag().keyword_boost_weight;
    enable_query_expansion_ = settings_obj->rag().enable_query_expansion;

    // Настройки глубокого анализа документа
    deep_analysis_mode_ = static_cast<int>(settings_obj->rag().deep_analysis.mode);
    deep_analysis_chunks_per_batch_ = settings_obj->rag().deep_analysis.chunks_per_batch;
    deep_analysis_max_iterations_ = settings_obj->rag().deep_analysis.max_iterations;
    deep_analysis_enable_progressive_summary_ = settings_obj->rag().deep_analysis.enable_progressive_summary;
    deep_analysis_final_synthesis_chunks_ = settings_obj->rag().deep_analysis.final_synthesis_chunks;
    deep_analysis_auto_adjust_context_size_ = settings_obj->rag().deep_analysis.auto_adjust_context_size;
    deep_analysis_target_context_size_ = settings_obj->rag().deep_analysis.target_context_size;

    // Настройки бесшовного расширения контекста
    enable_context_extension_ = settings_obj->rag().enable_context_extension;
    context_compression_threshold_ = settings_obj->rag().context_compression_threshold;
    keep_recent_messages_ = settings_obj->rag().keep_recent_messages;

    // Embedding Proxy settings
    enable_embedding_proxy_ = settings_obj->rag().enable_embedding_proxy;
    embedding_proxy_port_ = settings_obj->rag().embedding_proxy_port;

    // Копируем строку в буфер
    strncpy(embedding_model_path_buffer_, settings_obj->rag().embedding_model_path.c_str(), sizeof(embedding_model_path_buffer_) - 1);
    embedding_model_path_buffer_[sizeof(embedding_model_path_buffer_) - 1] = '\0';

    // URL сервера эмбеддингов (llama-server /v1/embeddings)
    strncpy(embedding_server_url_buffer_, settings_obj->rag().embedding_server_url.c_str(), sizeof(embedding_server_url_buffer_) - 1);
    embedding_server_url_buffer_[sizeof(embedding_server_url_buffer_) - 1] = '\0';
}

void RagSettingsDialog::render() {
    if (!visible_) {
        return;
    }

    // Приводим void* к Settings* для использования
    llama_gui::core::Settings* settings_obj = static_cast<llama_gui::core::Settings*>(settings_ptr_);

    if (ImGui::Begin(TR("rag_settings.title"), &visible_)) {
        ImGui::Text(TR("rag_settings.embedding_model"));
        ImGui::Separator();

        // Путь к модели эмбеддингов
        ImGui::Text(TR("rag_settings.embedding_model_path"));

        // Поле ввода пути (занимает часть строки)
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100);
        ImGui::InputText("##embedding_model_path", embedding_model_path_buffer_, sizeof(embedding_model_path_buffer_));

        ImGui::SameLine();

        // Кнопка выбора файла
        if (ImGui::Button(TR("button.browse"))) {
            // Устанавливаем флаг для открытия диалога из основного цикла
            pending_file_dialog_ = true;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("rag_settings.embedding_model_tooltip"));
        }

        ImGui::Spacing();
        ImGui::Separator();

        // URL сервера эмбеддингов (llama-server /v1/embeddings)
        ImGui::Text("URL сервера эмбеддингов (llama-server)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##embedding_server_url", embedding_server_url_buffer_, sizeof(embedding_server_url_buffer_));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("HTTP endpoint llama-server с моделью эмбеддингов.\n"
                              "Пример: http://localhost:8083\n"
                              "Запросы будут идти на <URL>/v1/embeddings.\n"
                              "Рекомендуется granite-embedding-107m-multilingual (384-dim).\n"
                              "Пусто = встроенный сервер запускается автоматически.\n"
                              "После смены сервера нужно переиндексировать профили!");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Embedding Proxy (OpenAI-compatible)");
        ImGui::Spacing();

        ImGui::Checkbox("Enable Embedding Proxy", &enable_embedding_proxy_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Launch a local HTTP proxy server that exposes an\n"
                              "OpenAI-compatible /v1/embeddings endpoint.\n"
                              "Cloud models (OpenAI, Cohere, etc.) can use this\n"
                              "to access your local embedding index.");
        }

        if (enable_embedding_proxy_) {
            ImGui::SetNextItemWidth(200);
            ImGui::InputInt("Proxy Port", &embedding_proxy_port_, 1, 10);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Port for the embedding proxy server.\n"
                                  "Default: 8082. Cloud models connect to\n"
                                  "http://localhost:<port>/v1/embeddings");
            }

            // Show connection info
            ImGui::TextDisabled("Endpoint: http://localhost:%d/v1/embeddings", embedding_proxy_port_);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text(TR("rag_settings.parameters"));
        ImGui::Spacing();

        // Максимальное количество чанков в памяти
        ImGui::SliderInt(TR("rag_settings.max_chunks"), &max_chunks_in_memory_, 100, 1000000);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("rag_settings.max_chunks_tooltip"));
        }

        // Порог схожести
        ImGui::SliderFloat(TR("rag_settings.similarity_threshold"), &similarity_threshold_, 0.3f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Порог схожести для RAG поиска (0.3-1.0). Меньшее значение (0.3-0.5) находит больше фрагментов, но менее релевантных. Большее значение (0.7-0.9) находит только самые релевантные фрагменты.");
        }

        // Максимальный размер кэша эмбеддингов
        ImGui::SliderInt(TR("rag_settings.max_cache_size"), &max_embedding_cache_size_, 10, 1000);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("rag_settings.max_cache_size_tooltip"));
        }

        // Максимальное количество токенов в чанке
        ImGui::SliderInt(TR("rag_settings.max_tokens_per_chunk"), &max_tokens_per_chunk_, 64, 2048);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("rag_settings.max_tokens_per_chunk_tooltip"));
        }

        // Количество результатов поиска RAG
        ImGui::SliderInt("Количество результатов поиска (k)", &search_k_, 1, 20);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Количество фрагментов документов для поиска в RAG. Меньшее значение (1-2) ускоряет обработку и снижает размер контекста. Большее значение (5-10) повышает точность и охват документа, но увеличивает время генерации. Для больших документов рекомендуется k=5-10.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("MMR (Maximal Marginal Relevance)");
        ImGui::Spacing();

        // Включение MMR
        ImGui::Checkbox("Включить MMR для разнообразия", &enable_mmr_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("MMR выбирает разнообразные результаты, избегая дублирования информации. Полезно для больших документов с повторяющимися темами.");
        }

        // MMR Lambda (только если MMR включен)
        if (enable_mmr_) {
            ImGui::SliderFloat("MMR Lambda (λ)", &mmr_lambda_, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Баланс между релевантностью и разнообразием:\n"
                                  "λ = 0.0 - только релевантность (как без MMR)\n"
                                  "λ = 0.5 - баланс (рекомендуется)\n"
                                  "λ = 1.0 - только разнообразие (может игнорировать запрос)");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Гибридный поиск (Vector + BM25)");
        ImGui::Spacing();

        // Включение гибридного поиска
        ImGui::Checkbox("Включить гибридный поиск", &enable_hybrid_search_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Гибридный поиск сочетает векторный поиск (семантический) с полнотекстовым поиском BM25.\nЭто улучшает точность поиска, особенно для специфичных терминов и имён собственных.");
        }

        // Вес ключевого слова (только если гибридный поиск включен)
        if (enable_hybrid_search_) {
            ImGui::SliderFloat("Вес ключевого слова (BM25)", &keyword_boost_weight_, 0.0f, 5.0f, "%.1f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Баланс между векторным и полнотекстовым поиском:\n"
                                  "0.0 - только векторный поиск\n"
                                  "1.0 - равный вес (рекомендуется)\n"
                                  "2.0-5.0 - больший вес у полнотекстового поиска\n"
                                  "Высокие значения лучше для имён, терминов, кода.");
            }

            ImGui::Spacing();
            ImGui::Checkbox("Расширение запроса (транслитерация)", &enable_query_expansion_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Автоматически добавляет транслитерированные варианты запроса.\nПолезно для поиска русскоязычных терминов в англоязычных документах и наоборот.\nПример: 'машинное обучение' → ['машинное обучение', 'mashinnoe obuchenie']");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Глубокий анализ документа (Map-Reduce)");
        ImGui::Spacing();

        // Выбор режима глубокого анализа
        ImGui::Text("Режим глубокого анализа:");
        static std::string mode_disabled = "Отключен (обычный RAG поиск)";
        static std::string mode_mapreduce = "Map-Reduce (рекомендуется для полных ответов)";
        static std::string mode_iterative = "Итеративный (последовательный обход)";
        static std::string mode_hierarchical = "Иерархический (для больших документов)";

        const char* deep_analysis_modes[] = {
            mode_disabled.c_str(),
            mode_mapreduce.c_str(),
            mode_iterative.c_str(),
            mode_hierarchical.c_str()
        };

        if (ImGui::Combo("##deep_analysis_mode", &deep_analysis_mode_, deep_analysis_modes, 4)) {
            // Режим изменён
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Глубокий анализ позволяет обработать ВЕСЬ документ целиком:\n"
                              "• Map-Reduce: разбивает на группы → создаёт резюме → синтезирует ответ\n"
                              "• Итеративный: последовательно обходит все чанки\n"
                              "• Иерархический: строит дерево резюме (для 1000+ чанков)");
        }

        // Дополнительные настройки (только если включен глубокий анализ)
        if (deep_analysis_mode_ != 0) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Параметры глубокого анализа:");

            ImGui::SliderInt("Чанков в батче", &deep_analysis_chunks_per_batch_, 5, 30);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Количество чанков в одной итерации (батче).\n"
                                  "Меньшее значение = больше итераций, но меньше риск превысить контекст.\n"
                                  "Большее значение = быстрее, но требует больше контекста.");
            }

            ImGui::SliderInt("Максимум итераций", &deep_analysis_max_iterations_, 10, 200);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Максимальное количество итераций обработки.\n"
                                  "Ограничивает время выполнения для очень больших документов.");
            }

            ImGui::Checkbox("Прогрессивное суммирование", &deep_analysis_enable_progressive_summary_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Построение итогового ответа через последовательное объединение промежуточных резюме.\n"
                                  "Рекомендуется для связности финального ответа.");
            }

            ImGui::SliderInt("Чанков для финального синтеза", &deep_analysis_final_synthesis_chunks_, 10, 100);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Количество промежуточных резюме для финального синтеза.\n"
                                  "Влияет на полноту и связность итогового ответа.");
            }

            ImGui::Separator();
            ImGui::Text("Настройки контекста:");

            ImGui::Checkbox("Авто-увеличение контекста сервера", &deep_analysis_auto_adjust_context_size_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Автоматически увеличивает n_ctx сервера при запуске глубокого анализа.\n"
                                  "Рекомендуется для обработки больших документов.");
            }

            if (deep_analysis_auto_adjust_context_size_) {
                ImGui::SliderInt("Целевой размер контекста", &deep_analysis_target_context_size_, 4096, 32768);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Целевой размер контекста сервера (n_ctx).\n"
                                      "4096 = ~15-20 чанков за раз\n"
                                      "8192 = ~30-40 чанков за раз\n"
                                      "16384 = ~60-80 чанков за раз\n"
                                      "32768 = ~120-150 чанков за раз");
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text(TR("rag_settings.features"));
        ImGui::Spacing();

        // Режим работы RAG
        ImGui::Separator();
        ImGui::Text(TR("rag_settings.mode"));

        // Сохраняем переводы в постоянные переменные, чтобы избежать проблемы с временными строками
        static std::string mode_documents_only = std::string(TR("rag.mode.documents_only"));
        static std::string mode_cache_only = std::string(TR("rag.mode.cache_only"));
        static std::string mode_both = std::string(TR("rag.mode.both"));

        const char* rag_modes[] = {
            mode_documents_only.c_str(),
            mode_cache_only.c_str(),
            mode_both.c_str()
        };

        if (ImGui::Combo("##rag_mode", &rag_mode_, rag_modes, 3)) {
            // Режим изменён
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("rag_settings.mode_tooltip"));
        }

        // Включение/выключение кэширования (зависит от режима)
        ImGui::Checkbox(TR("rag_settings.enable_caching"), &enable_caching_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("rag_settings.enable_caching_tooltip"));
        }

        ImGui::Spacing();
        ImGui::Separator();

        // === Настройки бесшовного расширения контекста ===
        ImGui::Text("Бесшовное расширение контекста");
        ImGui::Separator();

        ImGui::Checkbox("Включить расширение контекста", &enable_context_extension_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Автоматически сжимает старые сообщения диалога\n"
                              "при приближении к лимиту контекста модели.\n"
                              "Сжатые сообщения индексируются в RAG для поиска.");
        }

        if (enable_context_extension_) {
            ImGui::SliderInt("Порог сжатия (%)", &context_compression_threshold_, 70, 95);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Процент заполнения контекста для начала сжатия.\n"
                                  "70% = сжимать рано (больше деталей)\n"
                                  "85% = сжимать при необходимости\n"
                                  "95% = сжимать только в экстренных случаях");
            }

            ImGui::SliderInt("Сохранять последних сообщений", &keep_recent_messages_, 2, 10);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Количество последних сообщений, которые не суммаризируются.\n"
                                  "2 = агрессивное сжатие (много деталей в резюме)\n"
                                  "4 = сбалансированный режим\n"
                                  "10 = минимальное сжатие");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Кнопки OK и Cancel
        if (ImGui::Button(TR("button.ok"))) {
            // Сохраняем изменения в настройках
            llama_gui::core::RagSettings new_settings;
            new_settings.embedding_model_path = std::string(embedding_model_path_buffer_);
            new_settings.embedding_server_url = std::string(embedding_server_url_buffer_);
            new_settings.max_chunks_in_memory = max_chunks_in_memory_;
            new_settings.similarity_threshold = similarity_threshold_;
            new_settings.max_embedding_cache_size = max_embedding_cache_size_;
            new_settings.max_tokens_per_chunk = max_tokens_per_chunk_;
            new_settings.search_k = search_k_;
            new_settings.mmr_lambda = mmr_lambda_;
            new_settings.enable_mmr = enable_mmr_;
            new_settings.embedding_dimension = settings_obj->rag().embedding_dimension;
            new_settings.max_sequence_length = settings_obj->rag().max_sequence_length;
            new_settings.enable_caching = enable_caching_;
            new_settings.rag_mode = static_cast<llama_gui::core::RagMode>(rag_mode_);

            // Параметры гибридного поиска
            new_settings.enable_hybrid_search = enable_hybrid_search_;
            new_settings.keyword_boost_weight = keyword_boost_weight_;
            new_settings.enable_query_expansion = enable_query_expansion_;

            // Настройки глубокого анализа
            new_settings.deep_analysis.mode = static_cast<llama_gui::core::DeepAnalysisMode>(deep_analysis_mode_);
            new_settings.deep_analysis.chunks_per_batch = deep_analysis_chunks_per_batch_;
            new_settings.deep_analysis.max_iterations = deep_analysis_max_iterations_;
            new_settings.deep_analysis.enable_progressive_summary = deep_analysis_enable_progressive_summary_;
            new_settings.deep_analysis.final_synthesis_chunks = deep_analysis_final_synthesis_chunks_;
            new_settings.deep_analysis.auto_adjust_context_size = deep_analysis_auto_adjust_context_size_;
            new_settings.deep_analysis.target_context_size = deep_analysis_target_context_size_;

            // Настройки бесшовного расширения контекста
            new_settings.enable_context_extension = enable_context_extension_;
            new_settings.context_compression_threshold = context_compression_threshold_;
            new_settings.keep_recent_messages = keep_recent_messages_;

            // Embedding Proxy settings
            new_settings.enable_embedding_proxy = enable_embedding_proxy_;
            new_settings.embedding_proxy_port = embedding_proxy_port_;

            settings_obj->set_rag_settings(new_settings);
            
            // Сохраняем текущий профиль на диск
            std::string current_profile = settings_obj->get_current_profile_name();
            if (!current_profile.empty()) {
                if (settings_obj->save_profile(current_profile)) {
                    std::cout << "RAG settings saved to profile: " << current_profile << std::endl;
                } else {
                    std::cerr << "Failed to save RAG settings to profile" << std::endl;
                }
            } else {
                // Если профиль не загружен, сохраняем как "default"
                if (settings_obj->save_profile("default")) {
                    std::cout << "RAG settings saved to default profile" << std::endl;
                }
            }

            visible_ = false;
        }

        ImGui::SameLine();

        if (ImGui::Button(TR("button.cancel"))) {
            // Восстанавливаем значения из настроек
            strncpy(embedding_model_path_buffer_, settings_obj->rag().embedding_model_path.c_str(), sizeof(embedding_model_path_buffer_) - 1);
            embedding_model_path_buffer_[sizeof(embedding_model_path_buffer_) - 1] = '\0';
            strncpy(embedding_server_url_buffer_, settings_obj->rag().embedding_server_url.c_str(), sizeof(embedding_server_url_buffer_) - 1);
            embedding_server_url_buffer_[sizeof(embedding_server_url_buffer_) - 1] = '\0';
            max_chunks_in_memory_ = settings_obj->rag().max_chunks_in_memory;
            similarity_threshold_ = settings_obj->rag().similarity_threshold;
            max_embedding_cache_size_ = settings_obj->rag().max_embedding_cache_size;
            max_tokens_per_chunk_ = settings_obj->rag().max_tokens_per_chunk;
            search_k_ = settings_obj->rag().search_k;
            mmr_lambda_ = settings_obj->rag().mmr_lambda;
            enable_mmr_ = settings_obj->rag().enable_mmr;
            enable_caching_ = settings_obj->rag().enable_caching;
            rag_mode_ = static_cast<int>(settings_obj->rag().rag_mode);

            // Восстанавливаем параметры гибридного поиска
            enable_hybrid_search_ = settings_obj->rag().enable_hybrid_search;
            keyword_boost_weight_ = settings_obj->rag().keyword_boost_weight;
            enable_query_expansion_ = settings_obj->rag().enable_query_expansion;

            // Восстанавливаем настройки глубокого анализа
            deep_analysis_mode_ = static_cast<int>(settings_obj->rag().deep_analysis.mode);
            deep_analysis_chunks_per_batch_ = settings_obj->rag().deep_analysis.chunks_per_batch;
            deep_analysis_max_iterations_ = settings_obj->rag().deep_analysis.max_iterations;
            deep_analysis_enable_progressive_summary_ = settings_obj->rag().deep_analysis.enable_progressive_summary;
            deep_analysis_final_synthesis_chunks_ = settings_obj->rag().deep_analysis.final_synthesis_chunks;
            deep_analysis_auto_adjust_context_size_ = settings_obj->rag().deep_analysis.auto_adjust_context_size;
            deep_analysis_target_context_size_ = settings_obj->rag().deep_analysis.target_context_size;

            // Восстанавливаем настройки Embedding Proxy
            enable_embedding_proxy_ = settings_obj->rag().enable_embedding_proxy;
            embedding_proxy_port_ = settings_obj->rag().embedding_proxy_port;

            visible_ = false;
        }
    }
    ImGui::End();
}

void RagSettingsDialog::open_embedding_model_file_dialog() {
    if (!pending_file_dialog_ || !main_window_ptr_) {
        return;
    }

    // Сбрасываем флаг сразу - запускаем асинхронный выбор
    pending_file_dialog_ = false;

    MainWindow* main_window = static_cast<MainWindow*>(main_window_ptr_);
    main_window->open_embedding_model_picker([this](const std::string& path) {
        if (!path.empty()) {
            strncpy(embedding_model_path_buffer_, path.c_str(), sizeof(embedding_model_path_buffer_) - 1);
            embedding_model_path_buffer_[sizeof(embedding_model_path_buffer_) - 1] = '\0';
        }
    });
}

} // namespace ui
} // namespace llama_gui