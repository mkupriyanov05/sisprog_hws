#!/bin/bash
#
# create_user.sh - создаёт пользователя БЕЗ useradd, правкой системных файлов.
#   guest       - вход по SSH без пароля, $HOME смонтирован read-only
#   normal-pass - обычный пользователь с паролем
#   root        - пользователь с правами root (через группу sudo/wheel)
#
# Использование: sudo ./create_user.sh <username> <guest|normal-pass|root>
#

set -Eeuo pipefail

PASSWD=/etc/passwd
SHADOW=/etc/shadow
GROUP=/etc/group
GSHADOW=/etc/gshadow
SSHD_CONFIG=/etc/ssh/sshd_config
FSTAB=/etc/fstab
LOCKFILE=/etc/.pwd.lock

die() { echo "Ошибка: $*" >&2; exit 1; }

# ---------- Проверки аргументов ----------
[[ $EUID -eq 0 ]] || die "запустите скрипт от root (sudo)."
[[ $# -eq 2 ]]    || die "использование: $0 <username> <guest|normal-pass|root>"

USERNAME="$1"
MODE="$2"
HOME_DIR="/home/$USERNAME"

[[ "$USERNAME" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || die "недопустимое имя пользователя '$USERNAME'."
case "$MODE" in guest|normal-pass|root) ;; *) die "неизвестный режим '$MODE'. Допустимые: guest, normal-pass, root" ;; esac

# ---------- Вспомогательные функции ----------
detect_sudo_group() {
    if   getent group sudo  &>/dev/null; then echo sudo
    elif getent group wheel &>/dev/null; then echo wheel
    fi
}

detect_ssh_service() {
    local s
    for s in ssh sshd; do
        if systemctl cat "$s.service" &>/dev/null; then
            echo "$s"
            return
        fi
    done
}

# Добавить пользователя в список участников группы (4-е поле) атомарно:
# пишем во временный файл рядом, копируем права и владельца, затем mv.
add_member() {
    local file="$1" grp="$2" user="$3" tmp
    tmp=$(mktemp "$file.XXXXXX")
    awk -F: -v OFS=: -v g="$grp" -v u="$user" '
        $1 == g { if ($4 == "") $4 = u; else $4 = $4 "," u }
        { print }
    ' "$file" > "$tmp"
    chmod --reference="$file" "$tmp"
    chown --reference="$file" "$tmp"
    mv "$tmp" "$file"
}

# Запрос пароля до начала изменений; возвращает SHA-512 хэш.
ask_password_hash() {
    local p1 p2
    read -rsp "Введите пароль для '$USERNAME': " p1 </dev/tty; echo >&2
    read -rsp "Повторите пароль: " p2 </dev/tty; echo >&2
    [[ -n "$p1" ]]    || die "пустой пароль недопустим для этого режима."
    [[ "$p1" == "$p2" ]] || die "пароли не совпадают."
    printf '%s' "$p1" | openssl passwd -6 -stdin
}

# ---------- Пароль запрашиваем заранее (до блокировки) ----------
PASS_HASH=""
case "$MODE" in
    guest)                PASS_HASH="" ;;   # пустое поле = вход без пароля
    normal-pass|root)     PASS_HASH=$(ask_password_hash) ;;
esac

SUDO_GROUP=""
if [[ "$MODE" == "root" ]]; then
    SUDO_GROUP=$(detect_sudo_group)
    [[ -n "$SUDO_GROUP" ]] || die "не найдена группа sudo/wheel."
fi

# ---------- Блокировка ----------
# Все дальнейшие проверки и правки идут под блокировкой, чтобы между
# проверкой "нет такого пользователя" и записью никто не вмешался.
exec 9>"$LOCKFILE"
flock -x -w 15 9 || die "не удалось получить блокировку $LOCKFILE (занято другим процессом)."

# ---------- Бэкапы для отката ----------
BACKUP_DIR=$(mktemp -d)
HOME_CREATED=0
DONE=0
for f in "$PASSWD" "$SHADOW" "$GROUP" "$GSHADOW" "$SSHD_CONFIG" "$FSTAB"; do
    cp -p "$f" "$BACKUP_DIR/$(basename "$f")"
done

rollback() {
    local rc=$?
    if [[ $DONE -eq 0 ]]; then
        echo "Сбой (код $rc). Откатываю изменения..." >&2
        for f in "$PASSWD" "$SHADOW" "$GROUP" "$GSHADOW" "$SSHD_CONFIG" "$FSTAB"; do
            cp -p "$BACKUP_DIR/$(basename "$f")" "$f" 2>/dev/null || true
        done
        if mountpoint -q "$HOME_DIR" 2>/dev/null; then umount "$HOME_DIR" || true; fi
        if [[ $HOME_CREATED -eq 1 ]]; then rm -rf "$HOME_DIR"; fi
    fi
    rm -rf "$BACKUP_DIR"
}
trap rollback EXIT
trap 'exit 1' ERR

# ---------- Проверка, что имени нет ----------
grep -q "^$USERNAME:" "$PASSWD" && die "пользователь '$USERNAME' уже существует."
grep -q "^$USERNAME:" "$GROUP"  && die "группа '$USERNAME' уже существует."
[[ -e "$HOME_DIR" ]]            && die "директория $HOME_DIR уже существует."

# ---------- Выбор свободного UID/GID ----------
UID_MIN=$(awk '/^UID_MIN/ {print $2}' /etc/login.defs 2>/dev/null || true); UID_MIN=${UID_MIN:-1000}
UID_MAX=$(awk '/^UID_MAX/ {print $2}' /etc/login.defs 2>/dev/null || true); UID_MAX=${UID_MAX:-60000}

NEW_ID=""
for ((i = UID_MIN; i <= UID_MAX; i++)); do
    if ! awk -F: -v id="$i" '$3 == id {f=1} END {exit !f}' "$PASSWD" &&
       ! awk -F: -v id="$i" '$3 == id {f=1} END {exit !f}' "$GROUP"; then
        NEW_ID=$i
        break
    fi
done
[[ -n "$NEW_ID" ]] || die "нет свободного UID/GID в диапазоне $UID_MIN-$UID_MAX."

# ---------- 1. Личная группа ----------
echo "$USERNAME:x:$NEW_ID:" >> "$GROUP"
echo "$USERNAME:!::"        >> "$GSHADOW"

# ---------- 2. Запись в passwd ----------
echo "$USERNAME:x:$NEW_ID:$NEW_ID::$HOME_DIR:/bin/bash" >> "$PASSWD"

# ---------- 3. Запись в shadow ----------
LAST_CHANGE=$(( $(date +%s) / 86400 ))
echo "$USERNAME:$PASS_HASH:$LAST_CHANGE:0:99999:7:::" >> "$SHADOW"

# ---------- 4. Группа sudo/wheel (режим root) ----------
if [[ "$MODE" == "root" ]]; then
    add_member "$GROUP"   "$SUDO_GROUP" "$USERNAME"
    add_member "$GSHADOW" "$SUDO_GROUP" "$USERNAME"
fi

# ---------- 5. Домашняя директория ----------
mkdir "$HOME_DIR"
HOME_CREATED=1
if [[ "$MODE" != "guest" ]]; then
    cp -rT /etc/skel "$HOME_DIR"      # для гостя skel не копируем
    chmod 750 "$HOME_DIR"
fi
chown -R "$NEW_ID:$NEW_ID" "$HOME_DIR"

# ---------- 6. Особенности гостя ----------
if [[ "$MODE" == "guest" ]]; then
    chmod 500 "$HOME_DIR"
    find "$HOME_DIR" -mindepth 1 -type f -exec chmod 400 {} +
    find "$HOME_DIR" -mindepth 1 -type d -exec chmod 500 {} +

    # Пустой пароль разрешаем ТОЛЬКО этому пользователю
    if ! grep -qF "Match User $USERNAME" "$SSHD_CONFIG"; then
        printf '\nMatch User %s\n    PermitEmptyPasswords yes\n' "$USERNAME" >> "$SSHD_CONFIG"
    fi
    if command -v sshd &>/dev/null || [[ -x /usr/sbin/sshd ]]; then
        /usr/sbin/sshd -t || die "ошибка в sshd_config после изменений (изменения откатятся)."
    fi

    # Постоянное монтирование read-only
    FSTAB_LINE="$HOME_DIR $HOME_DIR none bind,ro,noexec,nosuid 0 0"
    grep -qF "$HOME_DIR $HOME_DIR" "$FSTAB" || echo "$FSTAB_LINE" >> "$FSTAB"
    systemctl daemon-reload
    mount --bind "$HOME_DIR" "$HOME_DIR"
    mount -o remount,ro,bind,noexec,nosuid "$HOME_DIR"

    SSH_SERVICE=$(detect_ssh_service)
    if [[ -n "$SSH_SERVICE" ]]; then
        systemctl restart "$SSH_SERVICE"
    else
        echo "Предупреждение: не удалось определить службу SSH, перезапустите её вручную." >&2
    fi
fi

DONE=1
echo "Пользователь '$USERNAME' создан (режим: $MODE, UID/GID: $NEW_ID)."
case "$MODE" in
    guest) echo "  - вход по SSH без пароля; \$HOME только для чтения (ro, noexec, nosuid)" ;;
    root)  echo "  - добавлен в группу '$SUDO_GROUP'" ;;
esac
echo "Проверка целостности: sudo pwck -r && sudo grpck -r"



