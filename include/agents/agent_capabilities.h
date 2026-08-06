#pragma once

#include <cstdint>
#include <type_traits>

namespace agents {

/**
 * @brief Возможности агента (битовая маска)
 * 
 * Определяет какие операции и ресурсы доступны агенту
 */
enum class AgentCapability : uint32_t {
    NONE              = 0,
    
    // Доступ к файловой системе
    FILE_READ         = (1 << 0),   ///< Чтение файлов
    FILE_WRITE        = (1 << 1),   ///< Запись файлов
    FILE_DELETE       = (1 << 2),   ///< Удаление файлов
    DIRECTORY_LIST    = (1 << 3),   ///< Список файлов в директории
    
    // Сетевые возможности
    HTTP_GET          = (1 << 4),   ///< HTTP GET запросы
    HTTP_POST         = (1 << 5),   ///< HTTP POST запросы
    HTTP_PUT          = (1 << 6),   ///< HTTP PUT запросы
    HTTP_DELETE       = (1 << 7),   ///< HTTP DELETE запросы
    
    // Вычисления и код
    CODE_GENERATION   = (1 << 8),   ///< Генерация кода
    CODE_ANALYSIS     = (1 << 9),   ///< Анализ кода
    CODE_EXECUTION    = (1 << 10),  ///< Выполнение кода (опасно!)
    
    // Терминал и процессы
    TERMINAL_EXEC     = (1 << 11),  ///< Выполнение команд терминала
    PROCESS_SPAWN     = (1 << 12),  ///< Запуск процессов
    
    // RAG и поиск
    RAG_SEARCH        = (1 << 13),  ///< Поиск по RAG
    EMBEDDING         = (1 << 14),  ///< Генерация эмбеддингов
    WEB_SEARCH        = (1 << 15),  ///< Поиск в интернете
    
    // Обработка текста
    TEXT_SUMMARY      = (1 << 16),  ///< Суммаризация текста
    TEXT_TRANSLATION  = (1 << 17),  ///< Перевод текста
    TEXT_EXTRACTION   = (1 << 18),  ///< Извлечение информации
    
    // Взаимодействие с другими агентами
    CALL_OTHER_AGENTS = (1 << 19),  ///< Вызов других агентов
    
    // Доступ к настройкам
    SETTINGS_READ     = (1 << 20),  ///< Чтение настроек
    SETTINGS_WRITE    = (1 << 21),  ///< Запись настроек
    
    // Специальные возможности
    LONG_RUNNING      = (1 << 22),  ///< Долгие операции (>30 сек)
    LARGE_OUTPUT      = (1 << 23),  ///< Большой вывод (>1MB)
    
    // Маски для группировки
    FILE_ALL          = FILE_READ | FILE_WRITE | FILE_DELETE | DIRECTORY_LIST,
    HTTP_ALL          = HTTP_GET | HTTP_POST | HTTP_PUT | HTTP_DELETE,
    CODE_ALL          = CODE_GENERATION | CODE_ANALYSIS | CODE_EXECUTION,
    TERMINAL_ALL      = TERMINAL_EXEC | PROCESS_SPAWN,
    RAG_ALL           = RAG_SEARCH | EMBEDDING,
    TEXT_ALL          = TEXT_SUMMARY | TEXT_TRANSLATION | TEXT_EXTRACTION,
};

/**
 * @brief Операции с битовой маской возможностей
 */
inline AgentCapability operator|(AgentCapability a, AgentCapability b) {
    using T = std::underlying_type_t<AgentCapability>;
    return static_cast<AgentCapability>(static_cast<T>(a) | static_cast<T>(b));
}

inline AgentCapability operator&(AgentCapability a, AgentCapability b) {
    using T = std::underlying_type_t<AgentCapability>;
    return static_cast<AgentCapability>(static_cast<T>(a) & static_cast<T>(b));
}

inline AgentCapability operator~(AgentCapability a) {
    using T = std::underlying_type_t<AgentCapability>;
    return static_cast<AgentCapability>(~static_cast<T>(a));
}

inline AgentCapability& operator|=(AgentCapability& a, AgentCapability b) {
    a = a | b;
    return a;
}

inline bool has_capability(AgentCapability mask, AgentCapability cap) {
    using T = std::underlying_type_t<AgentCapability>;
    return (static_cast<T>(mask) & static_cast<T>(cap)) != 0;
}

/**
 * @brief Преобразование возможности в строку
 */
inline const char* capability_to_string(AgentCapability cap) {
    switch (cap) {
        case AgentCapability::NONE: return "NONE";
        case AgentCapability::FILE_READ: return "FILE_READ";
        case AgentCapability::FILE_WRITE: return "FILE_WRITE";
        case AgentCapability::FILE_DELETE: return "FILE_DELETE";
        case AgentCapability::DIRECTORY_LIST: return "DIRECTORY_LIST";
        case AgentCapability::HTTP_GET: return "HTTP_GET";
        case AgentCapability::HTTP_POST: return "HTTP_POST";
        case AgentCapability::RAG_SEARCH: return "RAG_SEARCH";
        case AgentCapability::WEB_SEARCH: return "WEB_SEARCH";
        case AgentCapability::CODE_GENERATION: return "CODE_GENERATION";
        case AgentCapability::TERMINAL_EXEC: return "TERMINAL_EXEC";
        case AgentCapability::TEXT_SUMMARY: return "TEXT_SUMMARY";
        default: return "UNKNOWN";
    }
}

} // namespace agents
