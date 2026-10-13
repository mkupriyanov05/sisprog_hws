#!/bin/bash
# script_03_01.sh
# Запускает child-скрипт в фоне и убивает его через сигнал.
# Логирование выводится в терминал.
#
# Использование: ./script_03_01.sh [секунд_до_kill] [сигнал]
#   по умолчанию: 5 секунд, сигнал TERM
#   пример:       ./script_03_01.sh 3 KILL

DELAY="${1:-5}"
SIGNAL="${2:-TERM}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHILD="$DIR/child_03_01.sh"

log() { echo "[$(date '+%H:%M:%S')] [parent $$] $*"; }

if [[ ! -x "$CHILD" ]]; then
    log "ОШИБКА: $CHILD не найден или не исполняемый (chmod +x)"
    exit 1
fi

# Если прервали родителя (Ctrl+C) — не оставляем child-сироту
cleanup() {
    if kill -0 "$CHILD_PID" 2>/dev/null; then
        log "прерывание, убиваю child (PID=$CHILD_PID)"
        kill -TERM "$CHILD_PID"
        wait "$CHILD_PID" 2>/dev/null
    fi
    exit 130
}
trap cleanup INT

# Включаем job control: иначе bash запускает фоновые процессы с
# проигнорированными SIGINT/SIGQUIT, и child не сможет их получить.
set -m

log "запускаю child: $CHILD"
"$CHILD" &
CHILD_PID=$!
log "child запущен, PID=$CHILD_PID"

log "жду $DELAY сек. перед отправкой сигнала SIG$SIGNAL"
sleep "$DELAY"

if kill -0 "$CHILD_PID" 2>/dev/null; then
    log "отправляю SIG$SIGNAL процессу $CHILD_PID"
    kill -s "$SIGNAL" "$CHILD_PID"
else
    log "child уже завершился сам"
fi

wait "$CHILD_PID"
STATUS=$?

if (( STATUS > 128 )); then
    log "child завершён сигналом $((STATUS - 128)) (код выхода $STATUS)"
else
    log "child завершился с кодом $STATUS"
fi

log "готово"
