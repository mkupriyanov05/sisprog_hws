#!/bin/bash
#
# create_user.sh - создаёт пользователя в одном из трёх режимов:
#   normal      - обычный пользователь без пароля (заблокирован до установки пароля)
#   normal-pass - обычный пользователь с паролем (запрашивается интерактивно)
#   root        - пользователь с правами root (добавляется в sudo/wheel и UID=0 не используется,
#                 вместо этого пользователь добавляется в группу sudo/wheel)
#
# Использование:
#   sudo ./create_user.sh <username> <mode>
#   mode: normal | normal-pass | root
#

set -euo pipefail

# --- Проверка прав ---
if [[ $EUID -ne 0 ]]; then
    echo "Ошибка: скрипт должен быть запущен от root (используйте sudo)." >&2
    exit 1
fi

# --- Проверка аргументов ---
if [[ $# -ne 2 ]]; then
    echo "Использование: $0 <username> <normal|normal-pass|root>" >&2
    exit 1
fi

USERNAME="$1"
MODE="$2"

# --- Проверка имени пользователя ---
if ! [[ "$USERNAME" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]]; then
    echo "Ошибка: недопустимое имя пользователя '$USERNAME'." >&2
    exit 1
fi

if id "$USERNAME" &>/dev/null; then
    echo "Ошибка: пользователь '$USERNAME' уже существует." >&2
    exit 1
fi

# --- Определяем группу для sudo (зависит от дистрибутива) ---
detect_sudo_group() {
    if getent group sudo &>/dev/null; then
        echo "sudo"
    elif getent group wheel &>/dev/null; then
        echo "wheel"
    else
        echo ""
    fi
}

SSHD_CONFIG="/etc/ssh/sshd_config"

case "$MODE" in

    guest)
    	echo "Создаю гостевого пользователя '$USERNAME' (вход без пароля, \$HOME только для чтения)..."
    	useradd -m -s /bin/bash "$USERNAME"

    	# Убираем пароль полностью (пустое поле в /etc/shadow)
    	passwd -d "$USERNAME" >/dev/null

    	# --- Разрешаем пустой пароль ТОЛЬКО для этого пользователя ---
    	MATCH_BLOCK="Match User $USERNAME
	PermitEmptyPasswords yes"
    	    if ! grep -qF "Match User $USERNAME" "$SSHD_CONFIG"; then
        	{
            	echo ""
            	echo "$MATCH_BLOCK"
        	} >> "$SSHD_CONFIG"
    	    fi
    	systemctl restart ssh

    	# --- Настраиваем права перед монтированием ---
    	chown -R "$USERNAME:$USERNAME" "/home/$USERNAME"
    	find "/home/$USERNAME" -type f -exec chmod 400 {} \;
    	find "/home/$USERNAME" -type d -exec chmod 500 {} \;

    	# --- Монтируем $HOME гостя как read-only через bind mount ---
    	HOME_DIR="/home/$USERNAME"
    	mount --bind "$HOME_DIR" "$HOME_DIR"
    	mount -o remount,ro,bind,noexec,nosuid "$HOME_DIR"

	# Делаем монтирование постоянным (переживает перезагрузку)
        FSTAB_LINE="$HOME_DIR $HOME_DIR none bind,ro,noexec,nosuid 0 0"
        if ! grep -qF "$HOME_DIR $HOME_DIR" /etc/fstab; then
            echo "$FSTAB_LINE" >> /etc/fstab
        fi

        echo "Гость '$USERNAME' создан:"
        echo "  - вход по SSH без пароля (разрешено только для этого пользователя)"
        echo "  - \$HOME примонтирован read-only, noexec, nosuid - запись физически невозможна"
        ;;

    normal-pass)
        echo "Создаю обычного пользователя '$USERNAME' с паролем..."
        useradd -m -s /bin/bash "$USERNAME"
        echo "Введите пароль для '$USERNAME':"
        passwd "$USERNAME"
        echo "Пользователь '$USERNAME' создан с паролем."
        ;;

    root)
        SUDO_GROUP=$(detect_sudo_group)
        if [[ -z "$SUDO_GROUP" ]]; then
            echo "Ошибка: не найдена группа sudo/wheel в системе." >&2
            exit 1
        fi
        echo "Создаю пользователя '$USERNAME' с правами root (через группу '$SUDO_GROUP')..."
        useradd -m -s /bin/bash -G "$SUDO_GROUP" "$USERNAME"
        echo "Введите пароль для '$USERNAME':"
        passwd "$USERNAME"
        echo "Пользователь '$USERNAME' создан и добавлен в группу '$SUDO_GROUP'."
        echo "Внимание: пользователь получает root-права через sudo, а не через UID=0."
        ;;

    *)
        echo "Ошибка: неизвестный режим '$MODE'. Допустимые значения: normal, normal-pass, root" >&2
        exit 1
        ;;
esac

exit 0
