#!/usr/bin/env bash
# ============================================================================
# Локальный CI: сборка + unit-тесты + аудит UI (--audit-ui)
#
# Аудит проверяет консистентность меню/команд/хоткеев/окон и падает с кодом 2,
# если появились битые ссылки или мёртвые пункты меню (регрессия заглушек).
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"

echo "==> Конфигурация CMake ($BUILD)"
cmake -S "$ROOT" -B "$BUILD" >/dev/null

echo "==> Сборка"
cmake --build "$BUILD" -j"$(nproc)"

echo "==> Юнит-тесты (ctest)"
ctest --test-dir "$BUILD" --output-on-failure -j4

echo "==> Аудит UI (--audit-ui)"
REPORT="$BUILD/ui_audit_report.txt"
if "$BUILD/llama-gui-core" --audit-ui 2>&1 | tee "$REPORT"; then
    echo "CI OK: аудит чистый, отчёт: $REPORT"
else
    rc=$?
    if [ "$rc" -eq 2 ]; then
        echo "CI FAIL: аудит UI нашёл ошибки (см. $REPORT)" >&2
        grep -E "^ERROR" "$REPORT" >&2 || true
        exit 2
    fi
    exit "$rc"
fi
