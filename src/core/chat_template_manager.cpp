#include "../include/core/chat_template_manager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <cstdint>

namespace llama_gui {
namespace core {

// ============================================================================
// Singleton
// ============================================================================

ChatTemplateManager& ChatTemplateManager::instance() {
    static ChatTemplateManager instance;
    return instance;
}

// ============================================================================
// Специальные значения
// ============================================================================

bool ChatTemplateManager::is_special_value(const std::string& value) {
    return is_auto(value) || is_none(value) || is_file_path(value);
}

bool ChatTemplateManager::is_auto(const std::string& value) {
    return value == "auto" || value.empty();
}

bool ChatTemplateManager::is_none(const std::string& value) {
    return value == "none";
}

bool ChatTemplateManager::is_file_path(const std::string& value) {
    return value.size() > 5 && value.substr(0, 5) == "file:";
}

std::string ChatTemplateManager::get_file_path(const std::string& value) {
    if (is_file_path(value)) {
        return value.substr(5);
    }
    return value;
}

// ============================================================================
// Встроенные шаблоны
// ============================================================================

const std::unordered_map<std::string, std::string>& ChatTemplateManager::get_builtin_templates_map() {
    static const std::unordered_map<std::string, std::string> templates = {
        // LLaMA 2
        {"llama-2", "{% if messages[0]['role'] == 'system' %}{% set loop_messages = messages[1:] %}{% set system_message = messages[0]['content'] %}{% else %}{% set loop_messages = messages %}{% set system_message = false %}{% endif %}{% for message in loop_messages %}{% if (message['role'] == 'user') != (loop.index0 % 2 == 0) %}{{ raise_exception('Conversation roles must alternate user/assistant/user/assistant/...') }}{% endif %}{% if loop.index0 == 0 and system_message %}{% set content = '<<SYS>>\\n' + system_message + '\\n<</SYS>>\\n\\n' + message['content'] %}{% else %}{% set content = message['content'] %}{% endif %}{% if message['role'] == 'user' %}{{ bos_token + '[INST] ' + content.strip() + ' [/INST]' }}{% elif message['role'] == 'assistant' %}{{ ' ' + content.strip() + ' ' + eos_token }}{% endif %}{% endfor %}"},
        
        // LLaMA 3
        {"llama-3", "{% set loop_messages = messages %}{% for message in loop_messages %}{% if (message['role'] == 'user') != (loop.index0 % 2 == 0) %}{{ raise_exception('Conversation roles must alternate user/assistant/user/assistant/...') }}{% endif %}{% if loop.index0 == 0 and message['role'] == 'system' %}{% set system_message = message['content'] %}{% elif loop.index0 == 0 %}{% set system_message = false %}{% endif %}{% if message['role'] == 'user' %}{% if loop.index0 == 0 and system_message != false %}{{ '<|begin_of_text|><|start_header_id|>system<|end_header_id|>\\n\\n' + system_message + '<|eot_id|><|start_header_id|>user<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|><|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% else %}{{ '<|start_header_id|>user<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|><|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% endif %}{% elif message['role'] == 'assistant' %}{{ '<|start_header_id|>assistant<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|>' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% endif %}"},
        
        // LLaMA 3.1
        {"llama-3.1", "{% set loop_messages = messages %}{% for message in loop_messages %}{% if (message['role'] == 'user') != (loop.index0 % 2 == 0) %}{{ raise_exception('Conversation roles must alternate user/assistant/user/assistant/...') }}{% endif %}{% if loop.index0 == 0 and message['role'] == 'system' %}{% set system_message = message['content'] %}{% elif loop.index0 == 0 %}{% set system_message = false %}{% endif %}{% if message['role'] == 'user' %}{% if loop.index0 == 0 and system_message != false %}{{ '<|begin_of_text|><|start_header_id|>system<|end_header_id|>\\n\\n' + system_message + '<|eot_id|><|start_header_id|>user<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|><|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% else %}{{ '<|start_header_id|>user<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|><|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% endif %}{% elif message['role'] == 'assistant' %}{{ '<|start_header_id|>assistant<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|>' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% endif %}"},
        
        // Mistral Instruct
        {"mistral", "{% for message in messages %}{% if message['role'] == 'user' %}{{ '[INST] ' + message['content'] + ' [/INST]' }}{% elif message['role'] == 'assistant' %}{{ message['content'] + eos_token}}{% else %}{{ '[INST] ' + message['content'].strip() + ' [/INST]' }}{% endif %}{% endfor %}"},
        {"mistral-instruct", "{% for message in messages %}{% if message['role'] == 'user' %}{{ '[INST] ' + message['content'] + ' [/INST]' }}{% elif message['role'] == 'assistant' %}{{ message['content'] + eos_token}}{% else %}{{ '[INST] ' + message['content'].strip() + ' [/INST]' }}{% endif %}{% endfor %}"},
        
        // Qwen 2
        {"qwen-2", "{% for message in messages %}{% if loop.index0 == 0 %}{{ '<|im_start|>system\\n' + message['content'] + '<|im_end|>\\n' }}{% elif message['role'] == 'user' %}{{ '<|im_start|>user\\n' + message['content'] + '<|im_end|>\\n<|im_start|>assistant\\n' }}{% elif message['role'] == 'assistant' %}{{ message['content'] + '<|im_end|>\\n' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\\n' }}{% endif %}"},
        
        // Gemma
        {"gemma", "{% for message in messages %}{% if message['role'] == 'system' %}{{ '<bos>' + message['content'] }}{% elif message['role'] == 'user' %}{{ '<bos><start_of_turn>user\\n' + message['content'] + '<end_of_turn>\\n<start_of_turn>model\\n' }}{% elif message['role'] == 'assistant' %}{{ message['content'] + '<end_of_turn>\\n' }}{% endif %}{% endfor %}"},
        
        // Phi-3
        {"phi-3", "{% for message in messages %}{% if message['role'] == 'system' %}{{ '<|system|>\\n' + message['content'] + '<|end|>\\n' }}{% elif message['role'] == 'user' %}{{ '<|user|>\\n' + message['content'] + '<|end|>\\n' }}{% elif message['role'] == 'assistant' %}{{ '<|assistant|>\\n' + message['content'] + '<|end|>\\n' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|assistant|>\\n' }}{% endif %}"},
        
        // ChatGLM3
        {"chatglm3", "{% for message in messages %}{% if message['role'] == 'system' %}{{ '<|system|>\\n' + message['content'] }}{% elif message['role'] == 'user' %}{{ '<|user|>\\n' + message['content'] }}{% elif message['role'] == 'assistant' %}{{ '<|assistant|>\\n' + message['content'] }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|assistant|>\\n' }}{% endif %}"},
        
        // Yi
        {"yi", "{% for message in messages %}{% if message['role'] == 'system' %}{{ '<|im_start|>system\\n' + message['content'] + '<|im_end|>\\n' }}{% elif message['role'] == 'user' %}{{ '<|im_start|>user\\n' + message['content'] + '<|im_end|>\\n<|im_start|>assistant\\n' }}{% elif message['role'] == 'assistant' %}{{ message['content'] + '<|im_end|>\\n' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\\n' }}{% endif %}"},
        
        // OpenChat
        {"openchat", "{% for message in messages %}{% if message['role'] == 'system' %}{{ '<|start_header_id|>system<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|>' }}{% elif message['role'] == 'user' %}{{ '<|start_header_id|>user<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|>' }}{% elif message['role'] == 'assistant' %}{{ '<|start_header_id|>assistant<|end_header_id|>\\n\\n' + message['content'] + '<|eot_id|>' }}{% endif %}{% endfor %}{% if add_generation_prompt %}{{ '<|start_header_id|>assistant<|end_header_id|>\\n\\n' }}{% endif %}"},
        
        // Vicuna
        {"vicuna", "{% for message in messages %}{% if message['role'] == 'system' %}{{ 'SYSTEM: ' + message['content'] + '\\n' }}{% elif message['role'] == 'user' %}{{ 'USER: ' + message['content'] + '\\n' }}{% elif message['role'] == 'assistant' %}{{ 'ASSISTANT: ' + message['content'] + '\\n' }}{% endif %}{% endfor %}"}
    };
    return templates;
}

std::vector<std::string> ChatTemplateManager::get_builtin_template_names() const {
    std::vector<std::string> names;
    const auto& templates = get_builtin_templates_map();
    for (const auto& [name, _] : templates) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

ChatTemplateResult ChatTemplateManager::get_builtin_template(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    const auto& templates = get_builtin_templates_map();
    auto it = templates.find(name);
    if (it != templates.end()) {
        return ChatTemplateResult(it->second, "builtin");
    }
    
    return ChatTemplateResult::error("Unknown builtin template: " + name);
}

// ============================================================================
// Извлечение из GGUF
// ============================================================================

ChatTemplateResult ChatTemplateManager::extract_from_gguf(const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Проверка кэша
    auto cache_it = gguf_cache_.find(model_path);
    if (cache_it != gguf_cache_.end()) {
        return cache_it->second;
    }

    // Проверка существования файла
    std::ifstream file(model_path, std::ios::binary);
    if (!file.good()) {
        auto result = ChatTemplateResult::error("Model file not found: " + model_path);
        gguf_cache_[model_path] = result;
        return result;
    }

    // Чтение заголовка GGUF
    // Формат: magic (4 bytes), version (4 bytes), tensor_count (8 bytes), kv_count (8 bytes)
    char magic[4];
    file.read(magic, 4);
    if (!file.good() || magic[0] != 'G' || magic[1] != 'G' || magic[2] != 'U' || magic[3] != 'F') {
        auto result = ChatTemplateResult::error("Invalid GGUF file: " + model_path);
        gguf_cache_[model_path] = result;
        return result;
    }

    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), 4);

    uint64_t tensor_count, kv_count;
    file.read(reinterpret_cast<char*>(&tensor_count), 8);
    file.read(reinterpret_cast<char*>(&kv_count), 8);

    // Чтение key-value пар
    std::string chat_template;
    std::string model_name;

    for (uint64_t i = 0; i < kv_count; ++i) {
        // Чтение ключа
        uint64_t key_len;
        file.read(reinterpret_cast<char*>(&key_len), 8);
        if (!file.good()) break;
        
        // Защита от слишком больших ключей
        if (key_len > 1024) {
            file.seekg(key_len, std::ios::cur);
            continue;
        }
        
        std::string key(key_len, '\0');
        file.read(&key[0], key_len);

        // Чтение типа значения
        uint32_t value_type;
        file.read(reinterpret_cast<char*>(&value_type), 4);

        // Чтение значения в зависимости от типа
        // GGUF типы: 0=UINT8, 1=INT8, 2=UINT16, 3=INT16, 4=UINT32, 5=INT32, 6=FLOAT32, 7=BOOL, 8=STRING, 9=ARRAY, 10=UINT64, 11=INT64, 12=FLOAT64
        if (value_type == 8) { // GGUF_TYPE_STRING = 8 (не 1!)
            uint64_t value_len;
            file.read(reinterpret_cast<char*>(&value_len), 8);
            
            // Защита от слишком больших строк
            if (value_len > 10 * 1024 * 1024) { // 10MB limit
                file.seekg(value_len, std::ios::cur);
                continue;
            }
            
            std::string value(value_len, '\0');
            file.read(&value[0], value_len);

            if (key == "tokenizer.chat_template" || key == "chat_template") {
                chat_template = value;
            } else if (key == "general.name") {
                model_name = value;
            }
        } else {
            // Пропуск других типов
            // GGUF типы: 0=UINT8, 1=INT8, 2=UINT16, 3=INT16, 4=UINT32, 5=INT32, 6=FLOAT32, 7=BOOL, 8=STRING, 9=ARRAY, 10=UINT64, 11=INT64, 12=FLOAT64
            uint64_t skip_len = 0;
            switch (value_type) {
                case 0: case 1: case 7: // UINT8, INT8, BOOL
                    skip_len = 1; 
                    break;
                case 2: case 3: // UINT16, INT16
                    skip_len = 2; 
                    break;
                case 4: case 5: case 6: // UINT32, INT32, FLOAT32
                    skip_len = 4; 
                    break;
                case 10: case 11: case 12: // UINT64, INT64, FLOAT64
                    skip_len = 8; 
                    break;
                case 9: { // ARRAY - требует специальной обработки
                    // Читаем тип элементов (4 bytes) и количество элементов (8 bytes)
                    uint32_t element_type;
                    file.read(reinterpret_cast<char*>(&element_type), 4);
                    
                    uint64_t n_elements;
                    file.read(reinterpret_cast<char*>(&n_elements), 8);
                    
                    // Теперь пропускаем сами элементы
                    uint64_t element_size = 0;
                    switch (element_type) {
                        case 0: case 1: case 7: element_size = 1; break;
                        case 2: case 3: element_size = 2; break;
                        case 4: case 5: case 6: element_size = 4; break;
                        case 10: case 11: case 12: element_size = 8; break;
                        case 8: { // STRING - каждый элемент имеет длину
                            // Для массива строк нужно читать каждую строку отдельно
                            for (uint64_t j = 0; j < n_elements; ++j) {
                                uint64_t str_len;
                                file.read(reinterpret_cast<char*>(&str_len), 8);
                                if (str_len <= 10 * 1024 * 1024) {
                                    file.seekg(str_len, std::ios::cur);
                                }
                            }
                            element_size = 0; // Уже пропустили
                            break;
                        }
                        default: break;
                    }
                    
                    if (element_size > 0 && n_elements > 0) {
                        skip_len = element_size * n_elements;
                    } else {
                        skip_len = 0; // Уже обработано в случае STRING
                    }
                    break;
                }
                default: 
                    skip_len = 0; 
                    break;
            }
            if (skip_len > 0) {
                file.seekg(skip_len, std::ios::cur);
            }
        }
    }

    file.close();

    ChatTemplateResult result;
    if (!chat_template.empty()) {
        result.template_str = chat_template;
        result.source = "gguf";
        result.success = true;
        result.model_name = model_name;
    } else {
        result = ChatTemplateResult::error("No chat template found in GGUF metadata");
    }

    // Кэширование
    gguf_cache_[model_path] = result;
    return result;
}

// ============================================================================
// Загрузка из файла
// ============================================================================

ChatTemplateResult ChatTemplateManager::load_from_file(const std::string& template_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Проверка кэша
    auto cache_it = file_cache_.find(template_path);
    if (cache_it != file_cache_.end()) {
        return cache_it->second;
    }
    
    // Проверка существования файла
    std::ifstream file(template_path);
    if (!file.good()) {
        auto result = ChatTemplateResult::error("Template file not found: " + template_path);
        file_cache_[template_path] = result;
        return result;
    }
    
    // Чтение файла
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();
    
    if (content.empty()) {
        auto result = ChatTemplateResult::error("Template file is empty: " + template_path);
        file_cache_[template_path] = result;
        return result;
    }
    
    ChatTemplateResult result(content, "file");
    file_cache_[template_path] = result;
    return result;
}

// ============================================================================
// Эвристика по имени модели
// ============================================================================

std::string ChatTemplateManager::detect_from_model_name(const std::string& model_name) {
    std::string lower_name = model_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    // LLaMA 3.1
    if (lower_name.find("llama-3.1") != std::string::npos || 
        lower_name.find("llama3.1") != std::string::npos) {
        return "llama-3.1";
    }
    
    // LLaMA 3
    if (lower_name.find("llama-3") != std::string::npos || 
        lower_name.find("llama3") != std::string::npos) {
        return "llama-3";
    }
    
    // LLaMA 2
    if (lower_name.find("llama-2") != std::string::npos || 
        lower_name.find("llama2") != std::string::npos) {
        return "llama-2";
    }
    
    // Mistral
    if (lower_name.find("mistral") != std::string::npos) {
        if (lower_name.find("instruct") != std::string::npos) {
            return "mistral-instruct";
        }
        return "mistral";
    }
    
    // Qwen
    if (lower_name.find("qwen") != std::string::npos || 
        lower_name.find("qwq") != std::string::npos) {
        return "qwen-2";
    }
    
    // Gemma
    if (lower_name.find("gemma") != std::string::npos) {
        return "gemma";
    }
    
    // Phi
    if (lower_name.find("phi-3") != std::string::npos || 
        lower_name.find("phi3") != std::string::npos) {
        return "phi-3";
    }
    if (lower_name.find("phi-2") != std::string::npos || 
        lower_name.find("phi2") != std::string::npos) {
        return "phi-3"; // Phi-2 использует похожий формат
    }
    
    // ChatGLM
    if (lower_name.find("chatglm") != std::string::npos || 
        lower_name.find("glm") != std::string::npos) {
        return "chatglm3";
    }
    
    // Yi
    if (lower_name.find("yi-") != std::string::npos || 
        lower_name.find("yi_") != std::string::npos) {
        return "yi";
    }
    
    // OpenChat
    if (lower_name.find("openchat") != std::string::npos) {
        return "openchat";
    }
    
    // Vicuna
    if (lower_name.find("vicuna") != std::string::npos) {
        return "vicuna";
    }
    
    // Не найдено
    return "";
}

// ============================================================================
// Универсальный метод получения шаблона
// ============================================================================

ChatTemplateResult ChatTemplateManager::get_or_extract_template(
    const std::string& model_path, 
    const std::string& mode) 
{
    // "none" - отключить шаблон
    if (is_none(mode)) {
        ChatTemplateResult result;
        result.source = "none";
        result.success = true;
        return result;
    }
    
    // "auto" или пустой - извлечь из GGUF
    if (is_auto(mode)) {
        return extract_from_gguf(model_path);
    }
    
    // "file:/path" - загрузить из файла
    if (is_file_path(mode)) {
        return load_from_file(get_file_path(mode));
    }
    
    // Проверка на встроенный шаблон
    const auto& templates = get_builtin_templates_map();
    auto it = templates.find(mode);
    if (it != templates.end()) {
        return get_builtin_template(mode);
    }
    
    // Проверка на inline-шаблон (содержит {{ )
    if (mode.find("{{") != std::string::npos) {
        return ChatTemplateResult(mode, "inline");
    }
    
    // Попытка определить по имени модели
    std::string detected = detect_from_model_name(model_path);
    if (!detected.empty()) {
        auto result = get_builtin_template(detected);
        if (result.success) {
            result.source = "detected:" + detected;
        }
        return result;
    }
    
    // Неизвестный режим
    return ChatTemplateResult::error("Unknown chat template mode: " + mode);
}

// ============================================================================
// Очистка кэша
// ============================================================================

void ChatTemplateManager::clear_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    gguf_cache_.clear();
    file_cache_.clear();
}

} // namespace core
} // namespace llama_gui
