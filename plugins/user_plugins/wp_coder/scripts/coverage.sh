#!/usr/bin/env bash
# ============================================================================
# coverage.sh — покрытие core/ по строкам, с ПОРОГОМ как гейтом (И13.9)
#
# Задача 13.9 требовала «≥ 80 %» и не говорила, что именно это число
# проверяет. Критерий, который ничем не проверяется, протухает молча: цифра
# была бы верной в день написания задачи и осталась бы в плане навсегда.
# Поэтому здесь — настоящий гейт: ниже порога выход 1.
#
# Порядок именно такой, и каждая часть нужна:
#   1. отдельный каталог сборки ВНЕ репозитория — сборка in-source оставляет
#      следы, часть которых .gitignore прячет (правило 10 в SESSION_START_FAST);
#   2. --coverage при сборке И при компоновке: без второго счётчики не пишутся,
#      и отчёт получается пустым, то есть «покрытие 0 %» без причины;
#   3. -fno-inline: при инлайне строки заголовков уезжают в отчёт того .cpp,
#      который их позвал, и процент становится мерой того, как компилятор
#      решил Optimization, а не того, что покрыто тестами;
#   4. ПРОГОН ТЕСТОВ целиком: мерить нужно реальное поведение агента, а не
#      один удобный файл. Частичный прогон дал бы правдоподобную цифру
#      ниже настоящей.
#
# Время на этой машине (2 ядра, 7 ГБ) — около 25–35 минут, из них сборка
# почти всё. Это НЕ часть ежедневного прогона: гейт живёт отдельно, как
# контрольная пересборка с границы итерации (правило 3 в SESSION_START.md).
#
# Использование:
#   scripts/coverage.sh                     # порог 80 %
#   scripts/coverage.sh 90                  # другой порог (например, в гейт CI)
#   scripts/coverage.sh 80 --keep           # не удалять каталог сборки
#   WP_CODER_COV_DIR=/tmp/... scripts/coverage.sh
#
# Выход: 0 — порог взят (или не задан), 1 — покрытие ниже порога,
#         иная ненулевая — сборка/прогон не состоялись (это НЕ «покрыто мало»).
# ============================================================================
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COV_DIR="${WP_CODER_COV_DIR:-/tmp/wp_coder_coverage}"
THRESHOLD="${1:-80}"
KEEP=0
[ "${2:-}" = "--keep" ] && KEEP=1

# Порог 0 — это «просто отчёт»: гейтом он не является, и путать их нельзя,
# потому что exit 0 у обоих один и тот же.
if [ -z "${THRESHOLD}" ] || ! [ "${THRESHOLD}" -eq "${THRESHOLD}" ] 2>/dev/null; then
    echo "порог должен быть целым числом процентов, задано: '${THRESHOLD}'" >&2
    exit 2
fi

JOBS="$(nproc 2>/dev/null || echo 2)"

echo "==> Сборка с покрытием: ${COV_DIR} (${JOBS} потока)"
echo "    исходники: ${SRC}"
if ! cmake -S "${SRC}" -B "${COV_DIR}" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage -O0 -g -fno-inline" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
      -DCMAKE_SHARED_LINKER_FLAGS="--coverage" > "${COV_DIR}.cmake.log" 2>&1; then
    # Каталога может не быть — лог всё равно пишется, но make его не тронет.
    mkdir -p "${COV_DIR}"
    if ! cmake -S "${SRC}" -B "${COV_DIR}" \
          -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_CXX_FLAGS="--coverage -O0 -g -fno-inline" \
          -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
          -DCMAKE_SHARED_LINKER_FLAGS="--coverage" >> "${COV_DIR}.cmake.log" 2>&1; then
        echo "FAIL: конфигурация не прошла, лог: ${COV_DIR}.cmake.log" >&2
        exit 2
    fi
fi

if ! make -C "${COV_DIR}" wp_coder_tests -j"${JOBS}" > "${COV_DIR}.make.log" 2>&1; then
    echo "FAIL: сборка не прошла, лог: ${COV_DIR}.make.log" >&2
    exit 2
fi

echo "==> Прогон тестов (пишет .gcda)"
# Счётчики суммируются от запуска к запуску, а gcov честно показывает их
# суммой. Старые .gcda от прошлого прогона сделали бы процент завышенным, а
# завышенный процент — это ровно то, ради чего гейт и нужен.
find "${COV_DIR}" -name '*.gcda' -delete
if ! "${COV_DIR}/tests/wp_coder_tests"; then
    echo "FAIL: тесты не зелёные — покрытие измерять нечего" >&2
    exit 2
fi

echo "==> Покрытие core/ (порог ${THRESHOLD} %)"
python3 "${SRC}/scripts/measure_coverage.py" \
    --build "${COV_DIR}" \
    --source "${SRC}" \
    --json "${COV_DIR}/coverage.json" \
    --threshold "${THRESHOLD}"
RC=$?

if [ "${KEEP}" -eq 0 ]; then
    rm -rf "${COV_DIR}" "${COV_DIR}.cmake.log" "${COV_DIR}.make.log"
else
    echo "==> Каталог сохранён: ${COV_DIR}"
fi

exit ${RC}
