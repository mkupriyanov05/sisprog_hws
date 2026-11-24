/*
 * prog_04_02.c — создание / удаление файла с проверкой входных данных.
 *
 * Использование:
 *     prog_04_02 --create <путь_к_файлу>
 *     prog_04_02 --delete <путь_к_файлу>
 *
 * Проверки безопасности:
 *   1. Ровно два аргумента, режим — только --create или --delete.
 *   2. Санитизация пути: белый список символов [A-Za-z0-9._-/],
 *      непустой путь, длина < PATH_MAX, длина имени <= NAME_MAX,
 *      путь не заканчивается на '/', нет компонента "..".
 *   3. Каталог, в котором лежит файл, приводится к каноническому виду
 *      через realpath() (раскрываются симлинки) и сверяется
 *      с чёрным списком системных каталогов (/etc, /usr, /boot ...).
 *   4. Операции делаются через дескриптор каталога (openat / fstatat /
 *      unlinkat) с O_NOFOLLOW — это защищает от подмены файла симлинком.
 *   5. --create не перезаписывает существующий файл (O_EXCL).
 *   6. --delete удаляет только обычные файлы (не каталоги, не симлинки).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    EXIT_OK_    = 0,
    EXIT_FAIL   = 1,   /* ошибка операции (нет прав, файл уже есть и т.п.) */
    EXIT_USAGE  = 2,   /* неверные аргументы */
    EXIT_BADPATH = 3   /* путь не прошёл проверку безопасности */
};

/* Каталоги, в которых работать запрещено. "/" сравнивается только целиком. */
static const char *const FORBIDDEN_DIRS[] = {
    "/", "/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64",
    "/proc", "/root", "/run", "/sbin", "/sys", "/usr", "/var/lib",
    "/var/log", NULL
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Использование:\n"
            "  %s --create <файл>   создать пустой файл\n"
            "  %s --delete <файл>   удалить обычный файл\n",
            prog, prog);
}

/* Разрешены только: латиница, цифры, '.', '_', '-', '/'. */
static int is_allowed_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-' || c == '/';
}

/* Санитизация строки пути. 0 — ок, -1 — отклонено. */
static int validate_path(const char *p)
{
    size_t len = strlen(p);

    if (len == 0) {
        fprintf(stderr, "Ошибка: пустой путь.\n");
        return -1;
    }
    if (len >= PATH_MAX) {
        fprintf(stderr, "Ошибка: путь слишком длинный (макс. %d).\n", PATH_MAX - 1);
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_allowed_char((unsigned char)p[i])) {
            fprintf(stderr,
                    "Ошибка: недопустимый символ (код 0x%02x) в позиции %zu. "
                    "Разрешены: A-Z a-z 0-9 . _ - /\n",
                    (unsigned char)p[i], i + 1);
            return -1;
        }
    }
    if (p[len - 1] == '/') {
        fprintf(stderr, "Ошибка: путь должен указывать на файл, а не на каталог.\n");
        return -1;
    }

    /* Проверка каждого компонента пути. */
    const char *start = p;
    for (const char *c = p;; c++) {
        if (*c == '/' || *c == '\0') {
            size_t n = (size_t)(c - start);
            if (n == 2 && start[0] == '.' && start[1] == '.') {
                fprintf(stderr, "Ошибка: компонент \"..\" в пути запрещён.\n");
                return -1;
            }
            if (n > NAME_MAX) {
                fprintf(stderr, "Ошибка: слишком длинное имя (макс. %d).\n", NAME_MAX);
                return -1;
            }
            start = c + 1;
            if (*c == '\0')
                break;
        }
    }
    return 0;
}

/* Делит путь на каталог (в dir) и имя файла (возвращается указателем). */
static const char *split_path(const char *path, char dir[PATH_MAX])
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {                 /* "file.txt" */
        strcpy(dir, ".");
        return path;
    }
    if (slash == path) {                 /* "/file.txt" */
        strcpy(dir, "/");
        return slash + 1;
    }
    memcpy(dir, path, (size_t)(slash - path));   /* "a/b/file.txt" */
    dir[slash - path] = '\0';
    return slash + 1;
}

/* Каталог лежит внутри запрещённого? */
static int is_forbidden(const char *real_dir)
{
    for (int i = 0; FORBIDDEN_DIRS[i] != NULL; i++) {
        const char *f = FORBIDDEN_DIRS[i];
        size_t n = strlen(f);

        if (strcmp(f, "/") == 0) {
            if (strcmp(real_dir, "/") == 0)
                return 1;
            continue;
        }
        if (strncmp(real_dir, f, n) == 0 &&
            (real_dir[n] == '\0' || real_dir[n] == '/'))
            return 1;
    }
    return 0;
}

static int do_create(int dirfd, const char *real_dir, const char *name)
{
    int fd = openat(dirfd, name,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            fprintf(stderr, "Ошибка: '%s/%s' уже существует.\n", real_dir, name);
        else
            fprintf(stderr, "Ошибка создания '%s/%s': %s\n",
                    real_dir, name, strerror(errno));
        return EXIT_FAIL;
    }
    close(fd);
    printf("Создан файл: %s/%s\n", real_dir, name);
    return EXIT_OK_;
}

static int do_delete(int dirfd, const char *real_dir, const char *name)
{
    struct stat st;

    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT)
            fprintf(stderr, "Ошибка: '%s/%s' не существует.\n", real_dir, name);
        else
            fprintf(stderr, "Ошибка доступа к '%s/%s': %s\n",
                    real_dir, name, strerror(errno));
        return EXIT_FAIL;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr,
                "Отказ: '%s/%s' — не обычный файл (каталог, симлинк или спецфайл).\n",
                real_dir, name);
        return EXIT_BADPATH;
    }
    if (unlinkat(dirfd, name, 0) < 0) {
        fprintf(stderr, "Ошибка удаления '%s/%s': %s\n",
                real_dir, name, strerror(errno));
        return EXIT_FAIL;
    }
    printf("Удалён файл: %s/%s\n", real_dir, name);
    return EXIT_OK_;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    int create;
    if (strcmp(argv[1], "--create") == 0)
        create = 1;
    else if (strcmp(argv[1], "--delete") == 0)
        create = 0;
    else {
        fprintf(stderr, "Ошибка: неизвестный режим '%s'.\n", argv[1]);
        usage(argv[0]);
        return EXIT_USAGE;
    }

    const char *path = argv[2];
    if (validate_path(path) != 0)
        return EXIT_BADPATH;

    char dir[PATH_MAX];
    const char *name = split_path(path, dir);
    if (strcmp(name, ".") == 0) {
        fprintf(stderr, "Ошибка: недопустимое имя файла '.'.\n");
        return EXIT_BADPATH;
    }

    char real_dir[PATH_MAX];
    if (realpath(dir, real_dir) == NULL) {
        fprintf(stderr, "Ошибка: каталог '%s' недоступен: %s\n", dir, strerror(errno));
        return EXIT_BADPATH;
    }
    if (is_forbidden(real_dir)) {
        fprintf(stderr, "Отказ: работа в каталоге '%s' запрещена.\n", real_dir);
        return EXIT_BADPATH;
    }

    int dirfd = open(real_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        fprintf(stderr, "Ошибка открытия каталога '%s': %s\n", real_dir, strerror(errno));
        return EXIT_FAIL;
    }

    int rc = create ? do_create(dirfd, real_dir, name)
                    : do_delete(dirfd, real_dir, name);
    close(dirfd);
    return rc;
}
