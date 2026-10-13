#!/bin/bash
# Дочерний скрипт: работает в бесконечном цикле, пока его не убьют сигналом.

log() { echo "[$(date '+%H:%M:%S')] [child  $$] $*"; }

on_signal() {
    local sig="$1"
    log "получен сигнал SIG$sig, завершаюсь"
    exit $((128 + $(kill -l "$sig")))
}

# Перехватываем сигналы (KILL и STOP перехватить нельзя)
for sig in TERM INT HUP QUIT USR1 USR2; do
    trap "on_signal $sig" "$sig"
done

log "запущен (PID=$$)"
i=0
while true; do
    i=$((i + 1))
    log "работаю... итерация $i"
    sleep 1 &        # sleep в фоне + wait, чтобы сигнал обрабатывался сразу,
    wait $!          # а не после окончания sleep
done
