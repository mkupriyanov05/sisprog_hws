#!/bin/bash
#
# delete_guest.sh - удаляет гостевого пользователя, созданного create_user.sh
#   (размонтирует $HOME, чистит fstab и sshd_config, удаляет пользователя и его группу).
#
# Использование: sudo ./delete_guest.sh <username>
#

set -Eeuo pipefail

die() { echo "Ошибка: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "запустите скрипт от root (sudo)."
[[ $# -eq 1 ]]    || die "использование: $0 <username>"

USERNAME="$1"
HOME_DIR="/home/$USERNAME"
SSHD_CONFIG="/etc/ssh/sshd_config"
FSTAB="/etc/fstab"

[[ "$USERNAME" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || die "недопустимое имя пользователя '$USERNAME'."
id "$USERNAME" &>/dev/null || die "пользователь '$USERNAME' не существует."

# Защита: удаляем только гостей, созданных нашим скриптом
USER_UID=$(id -u "$USERNAME")
[[ $USER_UID -ge 1000 ]] || die "'$USERNAME' системный пользователь (UID < 1000), отказ."
grep -qF "Match User $USERNAME" "$SSHD_CONFIG" \
    || die "'$USERNAME' не похож на гостя (нет блока 'Match User $USERNAME' в sshd_config). Отказ."

detect_ssh_service() {
    local s
    for s in ssh sshd; do
        if systemctl cat "$s.service" &>/dev/null; then
            echo "$s"
            return
        fi
    done
}

# ---------- 1. Завершаем процессы пользователя ----------
if pgrep -u "$USERNAME" &>/dev/null; then
    echo "Завершаю процессы пользователя '$USERNAME'..."
    pkill -u "$USERNAME" || true
    sleep 1
    pkill -9 -u "$USERNAME" 2>/dev/null || true
fi

# ---------- 2. Размонтируем $HOME ----------
if mountpoint -q "$HOME_DIR"; then
    echo "Размонтирую $HOME_DIR..."
    umount "$HOME_DIR" || umount -l "$HOME_DIR"
fi

# ---------- 3. Чистим fstab ----------
if grep -qF "$HOME_DIR $HOME_DIR" "$FSTAB"; then
    echo "Удаляю строку из $FSTAB..."
    cp -p "$FSTAB" "$FSTAB.bak"
    sed -i "\#^$HOME_DIR $HOME_DIR #d" "$FSTAB"
    systemctl daemon-reload
fi

# ---------- 4. Чистим sshd_config ----------
echo "Удаляю Match-блок из $SSHD_CONFIG..."
cp -p "$SSHD_CONFIG" "$SSHD_CONFIG.bak"
sed -i "/^Match User $USERNAME\$/,+1d" "$SSHD_CONFIG"

if [[ -x /usr/sbin/sshd ]] && ! /usr/sbin/sshd -t; then
    echo "sshd_config стал некорректным, восстанавливаю из резервной копии." >&2
    cp -p "$SSHD_CONFIG.bak" "$SSHD_CONFIG"
    exit 1
fi

# ---------- 5. Удаляем пользователя, группу и домашнюю директорию ----------
echo "Удаляю пользователя '$USERNAME'..."
userdel -r "$USERNAME" 2>/dev/null || userdel "$USERNAME"
[[ -d "$HOME_DIR" ]] && rm -rf "$HOME_DIR"

# ---------- 6. Перезапускаем SSH ----------
SSH_SERVICE=$(detect_ssh_service)
if [[ -n "$SSH_SERVICE" ]]; then
    systemctl restart "$SSH_SERVICE"
else
    echo "Предупреждение: не удалось определить службу SSH, перезапустите её вручную." >&2
fi

echo "Гость '$USERNAME' удалён."
echo "Резервные копии: $FSTAB.bak, $SSHD_CONFIG.bak"
