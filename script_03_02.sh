#!/bin/bash
# script_03_02.sh
# Запускается в одном терминале и ловит сигналы, отправленные из другого.
# При получении сигнала печатает: signal <номер> catched...
#
# Использование:
#   Терминал 1:  ./script_03_02.sh
#   Терминал 2:  kill -USR1 <PID>   (или -HUP, -INT, -TERM, -9 и т.д.)
#
# TERM (15) — печатает сообщение и завершает скрипт.
# Остальные перехватываемые сигналы — печатают сообщение, скрипт продолжает работать.
# KILL (9) и STOP (19) перехватить нельзя — их ядро обрабатывает само.

SLEEP_PID=""

handler() {
    local num="$1"
    echo "signal $num catched... (SIG$(kill -l "$num"))"
    if [[ "$num" == "15" ]]; then
        echo "SIGTERM: завершаюсь"
        [[ -n "$SLEEP_PID" ]] && kill "$SLEEP_PID" 2>/dev/null
        exit 143
    fi
}

# Вешаем обработчик на сигналы 1..31, кроме:
#   9 (KILL), 19 (STOP) — перехватить невозможно,
#   17 (CHLD), 23 (URG), 28 (WINCH) — шумные, приходят сами по себе.
for num in $(seq 1 31); do
    case "$num" in
        9|17|19|23|28) continue ;;
    esac
    trap "handler $num" "$num"
done

echo "PID скрипта: $$"
echo "Отправьте сигнал из другого терминала, например:"
echo "    kill -USR1 $$      kill -HUP $$      kill -INT $$"
echo "    kill -TERM $$  (завершит скрипт)      kill -9 $$  (убьёт без перехвата)"
echo "Жду сигналов..."

# sleep в фоне + wait, чтобы сигнал обрабатывался сразу, а не после sleep
while true; do
    sleep 1 &
    SLEEP_PID=$!
    wait "$SLEEP_PID"
done
