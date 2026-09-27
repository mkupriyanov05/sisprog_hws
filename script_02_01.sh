#!/bin/bash
#
# Выводит последние 3 логина указанного пользователя (время + источник)
# Использование: ./script_02_01.sh <username>


# обработчик ошибок - закончить скрипт, если ошибка (-e);
# вызвать ошибку, если неизвестная переменная (-u)
set -euo pipefail


# вызвать ошибку, если больше одного аргумента
# $# - кол-во аргументов
# >&2 - stderr
# $0 - имя скрипта
if [[ $# -ne 1 ]]; then
    echo "Использование: $0 <username>" >&2
    exit 1
fi


# сохранить первый аргумент в переменную USERNAME
USERNAME="$1"


# поиск пользователя по id
if ! id "$USERNAME" &>/dev/null; then
    echo "Пользователь '$USERNAME' не найден в системе" >&2
    exit 1
fi


echo "Последние 3 входа пользователя '$USERNAME':"
echo "---------------------------------------------"

# На системах с wtmpdb (Ubuntu 24.04+) логины пишутся не в /var/log/wtmp,
# а в /var/lib/wtmpdb/wtmp.db через pam_wtmpdb.so — обычный `last` там пуст.

# -e '^$' убирает пустые строки
# -e '^wtmp begins' убирает строку начала журнала
if command -v wtmpdb &>/dev/null; then
    wtmpdb last -n 3 "$USERNAME"
else
    # -n 3 ограничивает вывод тремя записями
    last -n 3 "$USERNAME" | grep -v -e '^$' -e '^wtmp begins'
fi


