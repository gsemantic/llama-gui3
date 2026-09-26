# -*- mode: cmake -*-
#
# fetch_grammars.cmake — забрать грамматики tree-sitter из
# deps/grammars.lock. Запуск: make fetch-grammars
#
# Скрипт, а не цель в CMakeLists, по двум причинам. Он должен работать
# БЕЗ настроенной сборки (грамматика может понадобиться прежде, чем
# cmake вообще отработает), и он должен запускаться из уже настроенного
# дерева как обычная цель. `cmake -P` даёт и то, и другое.
#
# Переменные: GRAMMAR_LOCK (путь к lock-файлу), GRAMMAR_DEPS_DIR (куда
# класть), GRAMMAR_LANGUAGES (необязательный фильтр через «;»),
# GRAMMAR_FORCE=ON — перекачать даже если каталог уже есть.
#
# Корпуса (`test/`, `php/`, `php_only/`, `bindings/`) НЕ удаляются:
# сборка компилирует только `src/*.c`, а удалять файлы в чужом клоне
# молча — значит оставлять после себя грязное рабочее дерево. Сколько
# места освободить и как — печатается в конце.

if(NOT DEFINED GRAMMAR_LOCK)
  set(GRAMMAR_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/deps/grammars.lock")
endif()
if(NOT DEFINED GRAMMAR_DEPS_DIR)
  set(GRAMMAR_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps")
endif()

if(NOT EXISTS "${GRAMMAR_LOCK}")
  message(FATAL_ERROR "нет файла грамматик: ${GRAMMAR_LOCK}")
endif()

find_package(Git QUIET)
if(NOT GIT_FOUND)
  message(FATAL_ERROR
    "нужен git, чтобы забрать грамматики. Либо поставьте git, либо "
    "положите распакованные исходники в ${GRAMMAR_DEPS_DIR} "
    "вручную и пересоберите с -DWP_CODER_FETCH_GRAMMARS=OFF")
endif()

set(_langs "")
if(DEFINED GRAMMAR_LANGUAGES AND NOT GRAMMAR_LANGUAGES STREQUAL "")
  foreach(l IN LISTS GRAMMAR_LANGUAGES)
    list(APPEND _langs "${l}")
  endforeach()
endif()

set(_fetched 0)
set(_skipped 0)
set(_missing "")

# Разбор: сначала срезать комментарии, потом разбить на строки. Порядок
# обязателен — список CMake разделяется «;», и комментарий с точкой с
# запятой разорвал бы данные на части: первая версия не забрала ни одной
# грамматики и не сообщила ни об ошибке.
file(READ "${GRAMMAR_LOCK}" _raw)
string(REGEX REPLACE "#[^\n]*" "" _clean "${_raw}")
string(REPLACE "\n" ";" _lines "${_clean}")
foreach(_line IN LISTS _lines)
  string(STRIP "${_line}" _line)
  if(_line STREQUAL "")
    continue()
  endif()
  # Без \t и без {n,m}: регулярка CMake интервалов не знает, и с ними
  # схема не совпадала ни с одной строкой — забор проходил молча.
  if(NOT _line MATCHES "^([A-Za-z0-9_-]+) +([^ ]+) +([0-9a-fA-F]+)")
    message(WARNING "не разобрана строка lock-файла: ${_line}")
    continue()
  endif()
  set(_lang "${CMAKE_MATCH_1}")
  set(_url "${CMAKE_MATCH_2}")
  set(_sha "${CMAKE_MATCH_3}")
  string(LENGTH "${_sha}" _sha_len)
  if(NOT _sha_len EQUAL 40)
    message(FATAL_ERROR "в lock-файле ${_lang}: sha длиной ${_sha_len}, а нужен 40")
  endif()
  # Фильтр языков — через list(FIND), а не `if(x IN_LIST list)`:
  # в режиме `cmake -P` политика CMP0057 не задана, и IN_LIST не
  # распознаётся вовсе («Unknown arguments specified»). Сравнение строк
  # через STREQUAL с незаключённой переменной тоже схлопывалось, так что
  # обе формы молча пропускали весь забор.
  if(NOT "${_langs}" STREQUAL "")
    list(FIND _langs "${_lang}" _found)
    if(NOT _found EQUAL 0 AND NOT _found EQUAL 1 AND NOT _found EQUAL 2
       AND NOT _found EQUAL 3 AND NOT _found EQUAL 4
       AND NOT _found EQUAL 5 AND NOT _found EQUAL 6
       AND NOT _found EQUAL 7 AND NOT _found EQUAL 8)
      continue()
    endif()
  endif()

  set(_dest "${GRAMMAR_DEPS_DIR}/tree-sitter-${_lang}-src")
  if(EXISTS "${_dest}/src/parser.c" AND NOT GRAMMAR_FORCE)
    message(STATUS "уже на месте: ${_lang} (${_dest})")
    math(EXPR _skipped "${_skipped}+1")
    continue()
  endif()

  message(STATUS "забираю ${_lang} ${_sha}")
  file(REMOVE_RECURSE "${_dest}")
  # init + fetch по sha + checkout FETCH_HEAD, а не clone --branch:
  # clone умеет брать только теги и ветки, а пин здесь — это sha.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" init --quiet "${_dest}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "git init ${_dest} не удался: ${_err}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_dest}" remote add origin "${_url}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "remote add для ${_lang} не удался: ${_err}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_dest}" fetch --quiet --depth 1 origin "${_sha}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
  if(NOT _rc EQUAL 0)
    file(REMOVE_RECURSE "${_dest}")
    message(FATAL_ERROR
      "не удалось забрать ${_lang} (${_sha}) из ${_url}: ${_err}\n"
      "Вручную: git clone ${_url} ${_dest} && "
      "git -C ${_dest} checkout ${_sha}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_dest}" checkout --quiet FETCH_HEAD
    RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
  if(NOT _rc EQUAL 0)
    file(REMOVE_RECURSE "${_dest}")
    message(FATAL_ERROR "checkout ${_sha} для ${_lang} не удался: ${_err}")
  endif()
  if(NOT EXISTS "${_dest}/src/parser.c")
    message(FATAL_ERROR
      "в ${_dest} после забора нет src/parser.c — это не грамматика "
      "tree-sitter, версия не та")
  endif()
  math(EXPR _fetched "${_fetched}+1")
endforeach()

message(STATUS "грамматики: забрано ${_fetched}, уже было ${_skipped}")
message(STATUS
  "сборка использует только src/*.c; тестовые корпуса (test/, php/, "
  "php_only/, bindings/) можно удалить, если кончится место:\n"
  "  du -sh ${GRAMMAR_DEPS_DIR}/tree-sitter-*-src")
