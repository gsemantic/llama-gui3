#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Покрытие `core/` по данным gcov. И13.9.

Задача 13.9 говорит «≥ 80 %» и НЕ говорит, что считается процентом, поэтому
решение принято здесь и названо явно:

  единица — СТРОКА с исполняемым кодом (её видно в промежуточном формате gcov
  `-i` как запись в `lines`), а не функция и не ветка;
  в знаменателе — строки ВСЕХ `.cpp` из `CORE_SOURCES`, кроме файлов, которые
  перечислены в `--exclude` (по умолчанию пусто: исключать код, потому что он
  не покрыт, — значит врать о покрытии);
  строка покрыта, если ХОТЯ БЫ ОДИН её блок исполнялся: одна строка может
  попасть в отчёт несколько раз (перегрузки, шаблоны), и «все записи с
  нулевым счётом» не означает «строка не исполнялась».

Почему промежуточный формат, а не `gcov -l`: в текстовом отчёте счётчики
разъезжаются по строкам при `-fno-inline`, а в JSON счётчик один на запись.
Проверено на живой сборке, а не выбрано по описанию.

Скрипт НЕ собирает и НЕ запускает тесты — это делает `coverage.sh`. Разделение
по одной причине: пересборка занимает десятки минут, разбор — секунды, а
смешанный скрипт нельзя ни гонять часто, ни запускать вручную на готовых
данных.

Использование:
    measure_coverage.py --build <каталог сборки> [--source <корень плагина>]
                        [--threshold 80] [--json <файл>] [--exclude f.cpp,...]
"""

import argparse
import glob
import gzip
import json
import os
import subprocess
import sys


def run_gcov(build_dir, obj_subdir, out_dir):
    """Гоняет gcov по всем .gcda и возвращает список разобранных файлов."""
    pattern = os.path.join(build_dir, obj_subdir, "*.gcda")
    gcda_files = sorted(glob.glob(pattern))
    if not gcda_files:
        sys.exit(
            "нет ни одного .gcda в %s — тесты не были собраны с --coverage "
            "или не запускались" % os.path.join(build_dir, obj_subdir)
        )
    # gcov пишет отчёты в текущий каталог по basename исходника, поэтому
    # два исходника с одинаковым basename из разных каталогов затёрли бы
    # друг друга. Свой каталог на файл это снимает.
    parsed = []
    for gcda in gcda_files:
        own = os.path.join(out_dir, os.path.basename(gcda))
        os.makedirs(own, exist_ok=True)
        proc = subprocess.run(
            ["gcov", "-i", gcda],
            cwd=own,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if proc.returncode != 0:
            sys.exit(
                "gcov упал на %s (rc=%d):\n%s"
                % (gcda, proc.returncode, proc.stdout.decode("utf-8", "replace"))
            )
        for report in sorted(glob.glob(os.path.join(own, "*.gcov.json.gz"))):
            with gzip.open(report, "rt", encoding="utf-8") as fh:
                parsed.append(json.load(fh))
    return parsed


def merge(reports, source_dir, excludes):
    """Считает покрытие по строкам для файлов из `source_dir/core`."""
    core_prefix = os.path.join(os.path.abspath(source_dir), "core") + os.sep
    covered = {}
    total = {}
    for report in reports:
        for entry in report.get("files", []):
            path = entry.get("file", "")
            if not path.startswith(core_prefix) or not path.endswith(".cpp"):
                continue
            name = os.path.relpath(path, core_prefix)
            if name in excludes:
                continue
            # Строка может встретиться несколько раз: у неё несколько блоков
            # или несколько перегрузок. Покрыта она, если исполнялся хоть один.
            best = {}
            for line in entry.get("lines", []):
                num = line.get("line_number")
                if num is None:
                    continue
                best[num] = max(best.get(num, 0), line.get("count", 0))
            acc_c = covered.setdefault(name, {})
            acc_t = total.setdefault(name, {})
            for num, count in best.items():
                acc_t[num] = acc_t.get(num, 0) + 1
                acc_c[num] = max(acc_c.get(num, 0), count)
    rows = []
    for name in sorted(total):
        tot = len(total[name])
        cov = sum(1 for num in total[name] if covered[name].get(num, 0) > 0)
        rows.append((name, cov, tot, 100.0 * cov / tot if tot else 100.0))
    return rows


def main():
    ap = argparse.ArgumentParser(description="покрытие core/ по данным gcov")
    ap.add_argument("--build", required=True, help="каталог сборки с .gcda")
    ap.add_argument("--source", default=None, help="корень плагина (по умолчанию autodetect)")
    ap.add_argument("--obj", default="CMakeFiles/wp_coder_core.dir/core",
                    help="подкаталог с объектами ядра в каталоге сборки")
    ap.add_argument("--out", default=None, help="куда складывать отчёты gcov")
    ap.add_argument("--threshold", type=float, default=0.0,
                    help="порог в процентах; выше него — rc=1")
    ap.add_argument("--json", dest="json_out", default=None,
                    help="выписать разбор сюда (для машинного чтения)")
    ap.add_argument("--exclude", default="",
                    help="файлы core/ через запятую, которые НЕ считаются")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build)
    source_dir = args.source or os.environ.get("WP_CODER_SOURCE_DIR")
    if not source_dir:
        # Молчать здесь нельзя: без корня плагина отчёт собрался бы из пустого
        # набора и выглядел бы как «покрыто на 100 %».
        sys.exit("не указан --source и нет WP_CODER_SOURCE_DIR")

    out_dir = args.out or os.path.join(build_dir, "coverage_report")
    os.makedirs(out_dir, exist_ok=True)
    excludes = set(x for x in args.exclude.split(",") if x)

    reports = run_gcov(build_dir, args.obj, out_dir)
    rows = merge(reports, source_dir, excludes)

    # Незакрытые .cpp ядра обязаны быть ЗАМЕЧЕНЫ: молчаливый ноль строк от
    # файла выглядит как «покрыто на 100 %», и это ровно тот класс ошибки,
    # из-за которого критерий перестаёт быть критерием.
    #
    # Проверка идёт ПЕРЕД проверкой на пустой отчёт, а не после: при неверном
    # --source обе срабатывают, и тогда сообщение про «нет строк вообще» —
    # правда, но бесполезна. Имя файла из CMakeLists называет причину.
    known = set()
    cmake = os.path.join(source_dir, "CMakeLists.txt")
    with open(cmake, "r", encoding="utf-8") as fh:
        for line in fh:
            stripped = line.strip()
            if stripped.startswith("core/") and stripped.endswith(".cpp"):
                known.add(stripped[len("core/"):])
    missing = sorted(known - set(name for name, _, _, _ in rows) - excludes)
    if missing:
        sys.exit("в отчёте нет строк ни по одному файлу: %s" % ", ".join(missing))

    if not rows:
        sys.exit(
            "gcov не отдал ни одной строки из core/, а CMakeLists перечисляет %d "
            "файлов — проверь --source" % len(known)
        )

    total_lines = sum(t for _, _, t, _ in rows)
    covered_lines = sum(c for _, c, _, _ in rows)
    percent = 100.0 * covered_lines / total_lines if total_lines else 0.0

    width = max(len(name) for name, _, _, _ in rows)
    print("=== покрытие core/ по строкам (gcov -i) ===")
    for name, cov, tot, pct in rows:
        print("%-*s %5d/%-5d %6.2f %%" % (width, name, cov, tot, pct))
    print("-" * (width + 26))
    print("%-*s %5d/%-5d %6.2f %%" % (width, "ИТОГО", covered_lines, total_lines, percent))

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "covered": covered_lines,
                    "total": total_lines,
                    "percent": percent,
                    "excluded": sorted(excludes),
                    "files": [
                        {"file": n, "covered": c, "total": t, "percent": p}
                        for n, c, t, p in rows
                    ],
                },
                fh,
                indent=2,
                ensure_ascii=False,
            )

    if percent + 1e-9 < args.threshold:
        print(
            "ПРОВАЛ: %.2f %% < порога %.2f %%" % (percent, args.threshold),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
