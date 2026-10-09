#!/usr/bin/env bash
#
# script_02_03.sh - архивирует src_dir -> dst_dir согласно config,
#                   поддерживает выбор алгоритма сжатия и ротацию бэкапов.
#
# Использование:
#   ./script_02_03.sh -a <alg> [-c config] [-h]
#
#   -a <alg>    алгоритм архивации: tar.gz | tar.bz2 | tar.xz | zip
#   -c <file>   путь к конфигу (по умолчанию ./config)
#   -h          справка
#
# Формат config (key=value, пустые строки и строки с # игнорируются):
#   SRC_DIR=/var/log/myapp
#   DST_DIR=/var/backups/myapp
#   MAX_BACKUPS=5
#   POLICY=oldest      # oldest -> при превышении лимита удаляем самые старые
#                       # newest -> при превышении лимита удаляем самые новые
#
# Пример:
#   ./script_02_03.sh -a tar.gz -c ./config

set -euo pipefail

CONFIG_FILE="./config"
ALG=""

usage() {
    grep '^#' "$0" | sed -e '1d' -e 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# --- разбор аргументов ---
while getopts ":a:c:h" opt; do
    case "$opt" in
        a) ALG="$OPTARG" ;;
        c) CONFIG_FILE="$OPTARG" ;;
        h) usage 0 ;;
        \?) echo "Неизвестный флаг: -$OPTARG" >&2; usage 1 ;;
        :) echo "Флаг -$OPTARG требует аргумент" >&2; usage 1 ;;
    esac
done

if [[ -z "$ALG" ]]; then
    echo "Ошибка: не указан алгоритм (-a <alg>)" >&2
    usage 1
fi

case "$ALG" in
    tar.gz|tar.bz2|tar.xz|zip) ;;
    *)
        echo "Ошибка: неподдерживаемый алгоритм '$ALG'. Доступно: tar.gz, tar.bz2, tar.xz, zip" >&2
        exit 1
        ;;
esac

if [[ ! -f "$CONFIG_FILE" ]]; then
    echo "Ошибка: файл конфига '$CONFIG_FILE' не найден" >&2
    exit 1
fi

# --- чтение config ---
SRC_DIR=""
DST_DIR=""
MAX_BACKUPS=""
POLICY=""

while IFS='=' read -r key value; do
    [[ -z "$key" || "$key" =~ ^# ]] && continue
    key="$(echo "$key" | xargs)"
    value="$(echo "$value" | xargs)"
    case "$key" in
        SRC_DIR) SRC_DIR="$value" ;;
        DST_DIR) DST_DIR="$value" ;;
        MAX_BACKUPS) MAX_BACKUPS="$value" ;;
        POLICY) POLICY="$value" ;;
    esac
done < "$CONFIG_FILE"

# --- валидация ---
for var in SRC_DIR DST_DIR MAX_BACKUPS POLICY; do
    if [[ -z "${!var}" ]]; then
        echo "Ошибка: в конфиге не задан параметр $var" >&2
        exit 1
    fi
done

if [[ ! -d "$SRC_DIR" ]]; then
    echo "Ошибка: src dir '$SRC_DIR' не существует" >&2
    exit 1
fi

if [[ ! "$MAX_BACKUPS" =~ ^[0-9]+$ || "$MAX_BACKUPS" -lt 1 ]]; then
    echo "Ошибка: MAX_BACKUPS должен быть положительным числом" >&2
    exit 1
fi

if [[ "$POLICY" != "oldest" && "$POLICY" != "newest" ]]; then
    echo "Ошибка: POLICY должен быть oldest или newest" >&2
    exit 1
fi

mkdir -p "$DST_DIR"

# --- формируем имя архива ---
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
BASENAME="$(basename "$SRC_DIR")_${TIMESTAMP}"

case "$ALG" in
    tar.gz)  ARCHIVE="${DST_DIR}/${BASENAME}.tar.gz";  ARCHIVE_CMD=(tar -czf "$ARCHIVE" -C "$(dirname "$SRC_DIR")" "$(basename "$SRC_DIR")") ;;
    tar.bz2) ARCHIVE="${DST_DIR}/${BASENAME}.tar.bz2"; ARCHIVE_CMD=(tar -cjf "$ARCHIVE" -C "$(dirname "$SRC_DIR")" "$(basename "$SRC_DIR")") ;;
    tar.xz)  ARCHIVE="${DST_DIR}/${BASENAME}.tar.xz";  ARCHIVE_CMD=(tar -cJf "$ARCHIVE" -C "$(dirname "$SRC_DIR")" "$(basename "$SRC_DIR")") ;;
    zip)     ARCHIVE="${DST_DIR}/${BASENAME}.zip";     ARCHIVE_CMD=(zip -rq "$ARCHIVE" "$SRC_DIR") ;;
esac

# --- ротация бэкапов (ДО создания нового архива) ---
# Освобождаем место так, чтобы после создания нового архива их стало не больше MAX_BACKUPS.
# Новый архив ещё не существует, поэтому случайно удалить его невозможно.
mapfile -t BACKUPS < <(find "$DST_DIR" -maxdepth 1 -type f -name "$(basename "$SRC_DIR")_*" ! -path "$ARCHIVE" -printf '%T@ %p\n' | sort -n | awk '{print $2}')

COUNT=${#BACKUPS[@]}
echo "Текущее число бэкапов: $COUNT (лимит: $MAX_BACKUPS)"

EXCESS=$(( COUNT - MAX_BACKUPS + 1 ))
if (( EXCESS > 0 )); then
    echo "Нужно освободить место, удаляю бэкапов: $EXCESS (policy: $POLICY)"

    if [[ "$POLICY" == "oldest" ]]; then
        # BACKUPS отсортирован от старых к новым -> берём с начала
        TO_DELETE=("${BACKUPS[@]:0:EXCESS}")
    else
        # newest -> берём самые новые из уже существующих
        TO_DELETE=("${BACKUPS[@]: -EXCESS}")
    fi

    for f in "${TO_DELETE[@]}"; do
        echo "Удаляю: $f"
        rm -f "$f"
    done
fi

# --- создание архива ---
echo "Создаю архив: $ARCHIVE"
"${ARCHIVE_CMD[@]}"
echo "Архив создан."


echo "Готово."
