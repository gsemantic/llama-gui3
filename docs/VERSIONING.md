# Система версионирования Llama GUI

## Обзор

Проект использует систему версионирования на основе **SemVer 2.0.0** (Semantic Versioning).

## Формат версии

```
MAJOR.MINOR.PATCH-PRERELEASE "CODENAME"
```

### Примеры
- `0.1.0-alpha.1 "Archimedes"` - Первая альфа-версия
- `0.1.0-alpha.2 "Archimedes"` - Вторая альфа-версия (текущая)
- `0.1.0-beta.1 "Bernoulli"` - Первая бета-версия
- `1.0.0 "Newton"` - Стабильный релиз

## Компоненты версии

| Компонент | Описание | Когда изменять |
|-----------|----------|----------------|
| **MAJOR** | Основная версия | При несовместимых изменениях API |
| **MINOR** | Минорная версия | При новой функциональности (обратно совместимой) |
| **PATCH** | Патч | При исправлении багов (обратно совместимых) |
| **PRERELEASE** | Пре-релиз | `alpha.X`, `beta.X`, `rc.X` |
| **CODENAME** | Кодовое имя | Для каждой основной версии |

## Как обновить версию

### 1. Откройте `CMakeLists.txt`

Найдите секцию версионирования (строки 5-11):

```cmake
set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX "alpha.2")
set(VERSION_CODENAME "Archimedes")
```

### 2. Измените значения

#### Для альфа-версии:
```cmake
set(VERSION_SUFFIX "alpha.3")  # Увеличьте номер альфы
```

#### Для бета-версии:
```cmake
set(VERSION_PATCH 0)
set(VERSION_SUFFIX "beta.1")   # Сбросьте PATCH, начните бета
set(VERSION_CODENAME "Bernoulli")
```

#### Для стабильного релиза:
```cmake
set(VERSION_MAJOR 1)           # Увеличьте MAJOR
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX "")         # Пусто для стабильной версии
set(VERSION_CODENAME "Newton")
```

### 3. Пересоберите проект

```bash
cd build
cmake ..
make -j4
```

## Автоматически генерируемая информация

Следующие данные определяются автоматически при сборке:

- **BUILD_DATE** - Дата сборки (ГГГГ-ММ-ДД)
- **BUILD_TIME** - Время сборки (ЧЧ:ММ:СС)
- **BUILD_YEAR** - Год сборки
- **BUILD_TYPE** - Тип сборки (Debug/Release)
- **COMPILER_VERSION** - Версия компилятора
- **GIT_COMMIT_HASH** - Хэш коммита Git
- **GIT_DESCRIBE** - Описание Git (тег + коммиты)

## Где используется версия

### 1. В консоли при запуске
```
======================================================
        Llama.cpp C++ GUI - ЗАПУСК
======================================================
  Версия: 0.1.0-alpha.2 Archimedes
  Сборка: 2026-03-17 20:12:20
  Git:    abc1234
======================================================
```

### 2. В окне "О программе"
Меню **Справка → О программе**

### 3. В version.json
Файл в директории сборки содержит полную информацию о версии и сборке.

### 4. В заголовке окна
Отображается в строке заголовка главного окна.

## API для работы с версией

### C++ код

```cpp
#include "core/version.h"

using namespace llama_gui::core;

// Получить полную версию
const char* version = getVersionFull();  // "0.1.0-alpha.2 Archimedes"

// Получить компоненты
int major = getVersionMajor();   // 0
int minor = getVersionMinor();   // 1
int patch = getVersionPatch();   // 0

// Получить метаданные сборки
const char* buildDate = getBuildDate();      // "2026-03-17"
const char* gitHash = getGitCommitHash();    // "abc1234"

// Проверить пре-релиз
bool isPre = isPrerelease();   // true для 0.x.x
```

### CMake

```cmake
message(STATUS "Version: ${VERSION_FULL}")
message(STATUS "Build: ${BUILD_DATE} ${BUILD_TIME}")
```

## Кодовые имена версий

Предлагается использовать имена великих учёных и математиков:

| Версия | Кодовое имя | Описание |
|--------|-------------|----------|
| 0.1.x | Archimedes | Архимед - основы |
| 0.2.x | Bernoulli | Бернулли - вероятность |
| 0.3.x | Cantor | Кантор - множества |
| 1.0.0 | Newton | Ньютон - стабильность |
| 1.1.0 | Darwin | Дарвин - эволюция |
| 2.0.0 | Einstein | Эйнштейн - революция |

## История версий

### 0.1.0-alpha.2 "Archimedes" (текущая)
- Добавлена система примагничивания окон по сетке (Grid Snapping)
- Добавлена система версионирования
- Улучшен диалог "О программе"

### 0.1.0-alpha.1 "Archimedes"
- Первая рабочая версия
- Базовый функционал чата
- Интеграция с llama.cpp
- Система агентов

## Ссылки

- [SemVer 2.0.0 Specification](https://semver.org/)
- [GNU Coding Standards - Version Numbers](https://www.gnu.org/prep/standards/html_node/Version-Numbers.html)
