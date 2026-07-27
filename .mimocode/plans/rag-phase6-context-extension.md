# Phase 6: Seamless Context Extension (RAG)

## Цель
Расширение контекстного окна за счёт суммаризации старых сообщений и индексации их в RAG.
Вместо жёсткого обрезания (`max_messages=10`) — сжатие + индексация + поиск по demand.

## Архитектура

### ContextMonitor (include/core/context_monitor.h)
Отслеживает использование контекста:
- Подсчёт токенов в диалоге (приближённый)
- Пороговые значения: warning (70%), critical (85%), overflow (95%)
- Сигналы для ContextCompressor

### ContextCompressor (include/core/context_compressor.h)
Автосуммаризация старых сообщений:
- Группирует старые сообщения в батчи
- Отправляет в LLM для суммаризации через LlamaInterface
- Возвращает компактные резюме
- Индексирует резюме в RAG

### Интеграция в ChatInterface
- ContextMonitor проверяет размер контекста перед отправкой
- При превышении порога — ContextCompressor суммаризует старые сообщения
- Суммаризации индексируются в RagManager
- При поиске — RAG находит релевантные суммаризации

## Файлы для создания
1. `include/core/context_monitor.h` — ContextMonitor class
2. `src/core/context_monitor.cpp` — реализация
3. `include/core/context_compressor.h` — ContextCompressor class
4. `src/core/context_compressor.cpp` — реализация

## Файлы для модификации
1. `include/core/rag_manager.h` — добавить методы для индексации суммаризаций
2. `src/core/rag_manager_core.cpp` — реализация
3. `include/ui/chat_interface.h` — добавить ContextMonitor/ContextCompressor
4. `src/ui/chat_message_send.cpp` — интеграция перед отправкой
5. `CMakeLists.txt` — добавить новые файлы

## Порядок реализации
1. ContextMonitor (простой, без зависимостей)
2. ContextCompressor (зависит от LlamaInterface)
3. Расширение RagManager (методы для суммаризаций)
4. Интеграция в ChatInterface
5. Тесты
