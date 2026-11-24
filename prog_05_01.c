/*
 * prog_05_01.c
 *
 * Password keeper на основе стека.
 *  - стек реализован на массиве (Entry *data + size + capacity)
 *  - динамическая память: malloc() / free()
 *  - cli-меню: push / pop / peek / list / search / size
 *  - search: просмотр конкретной записи по названию сервиса
 *
 * Сборка:  gcc -Wall -Wextra -o prog_05_01 prog_05_01.c
 * Запуск:  ./prog_05_01
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define INITIAL_CAPACITY 4

/* ---------- Структуры ---------- */

typedef struct {
    char *service;
    char *login;
    char *password;
} Entry;

typedef struct {
    Entry  *data;      /* массив записей в куче */
    size_t  size;      /* сколько записей сейчас */
    size_t  capacity;  /* сколько влезает без расширения */
} Stack;

/* ---------- Вспомогательные функции ---------- */

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        fprintf(stderr, "Ошибка: не хватает памяти\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

/* копия строки через malloc (аналог strdup) */
static char *copy_str(const char *s)
{
    size_t len = strlen(s);
    char *p = xmalloc(len + 1);
    memcpy(p, s, len + 1);
    return p;
}

/* читает строку из stdin, убирает '\n'. Возвращает malloc-строку или NULL при EOF */
static char *read_line(const char *prompt)
{
    char buf[512];

    if (prompt != NULL) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if (fgets(buf, sizeof(buf), stdin) == NULL)
        return NULL;

    buf[strcspn(buf, "\n")] = '\0';

    /* если строка не влезла в буфер - вычищаем остаток из stdin */
    if (strlen(buf) == sizeof(buf) - 1) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
    return copy_str(buf);
}

/* ввод пароля без эха (если stdin - терминал) */
static char *read_password(const char *prompt)
{
    struct termios old, hidden;
    int is_tty = isatty(STDIN_FILENO);

    if (is_tty) {
        tcgetattr(STDIN_FILENO, &old);
        hidden = old;
        hidden.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
    }

    char *pass = read_line(prompt);

    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        printf("\n");
    }
    return pass;
}

static void free_entry(Entry *e)
{
    free(e->service);
    free(e->login);
    free(e->password);
    e->service = e->login = e->password = NULL;
}

/* ---------- Операции стека ---------- */

static void stack_init(Stack *s)
{
    s->capacity = INITIAL_CAPACITY;
    s->size = 0;
    s->data = xmalloc(s->capacity * sizeof(Entry));
}

static int stack_is_empty(const Stack *s)
{
    return s->size == 0;
}

/* расширение массива вдвое: malloc нового, копирование, free старого */
static void stack_grow(Stack *s)
{
    size_t new_cap = s->capacity * 2;
    Entry *new_data = xmalloc(new_cap * sizeof(Entry));

    memcpy(new_data, s->data, s->size * sizeof(Entry));
    free(s->data);

    s->data = new_data;
    s->capacity = new_cap;
}

/* кладёт запись на вершину. Стек забирает владение строками */
static void stack_push(Stack *s, Entry e)
{
    if (s->size == s->capacity)
        stack_grow(s);
    s->data[s->size++] = e;
}

/* снимает запись с вершины. Вызывающий должен сам освободить её (free_entry) */
static int stack_pop(Stack *s, Entry *out)
{
    if (stack_is_empty(s))
        return 0;
    *out = s->data[--s->size];
    return 1;
}

static const Entry *stack_peek(const Stack *s)
{
    if (stack_is_empty(s))
        return NULL;
    return &s->data[s->size - 1];
}

static void stack_destroy(Stack *s)
{
    for (size_t i = 0; i < s->size; i++)
        free_entry(&s->data[i]);
    free(s->data);
    s->data = NULL;
    s->size = s->capacity = 0;
}

/* ---------- Вывод ---------- */

static void print_entry(size_t index, const Entry *e, int show_password)
{
    printf("  [%zu] Сервис: %s\n", index, e->service);
    printf("      Логин:  %s\n", e->login);
    if (show_password)
        printf("      Пароль: %s\n", e->password);
    else
        printf("      Пароль: ********\n");
}

/* ---------- Действия меню ---------- */

static void action_push(Stack *s)
{
    Entry e;

    e.service  = read_line("Сервис:  ");
    e.login    = read_line("Логин:   ");
    e.password = read_password("Пароль:  ");

    if (e.service == NULL || e.login == NULL || e.password == NULL) {
        free(e.service);
        free(e.login);
        free(e.password);
        printf("Ввод прерван, запись не добавлена.\n");
        return;
    }
    if (e.service[0] == '\0') {
        free_entry(&e);
        printf("Название сервиса не может быть пустым.\n");
        return;
    }

    stack_push(s, e);
    printf("Запись добавлена (всего: %zu).\n", s->size);
}

static void action_pop(Stack *s)
{
    Entry e;

    if (!stack_pop(s, &e)) {
        printf("Стек пуст.\n");
        return;
    }
    printf("Удалена верхняя запись:\n");
    print_entry(s->size, &e, 0);
    free_entry(&e);
}

static void action_peek(const Stack *s)
{
    const Entry *e = stack_peek(s);

    if (e == NULL) {
        printf("Стек пуст.\n");
        return;
    }
    printf("Верхняя запись:\n");
    print_entry(s->size - 1, e, 1);
}

static void action_list(const Stack *s)
{
    if (stack_is_empty(s)) {
        printf("Стек пуст.\n");
        return;
    }
    printf("Записи (сверху вниз, пароли скрыты):\n");
    for (size_t i = s->size; i > 0; i--)
        print_entry(i - 1, &s->data[i - 1], 0);
}

/* search: ищет записи, в названии сервиса которых есть подстрока (без учёта регистра),
 * и показывает их вместе с паролем */
static void action_search(const Stack *s)
{
    if (stack_is_empty(s)) {
        printf("Стек пуст.\n");
        return;
    }

    char *query = read_line("Название сервиса для поиска: ");
    if (query == NULL || query[0] == '\0') {
        free(query);
        printf("Пустой запрос.\n");
        return;
    }

    size_t found = 0;
    for (size_t i = s->size; i > 0; i--) {
        const Entry *e = &s->data[i - 1];
        if (strcasestr(e->service, query) != NULL) {
            print_entry(i - 1, e, 1);
            found++;
        }
    }
    if (found == 0)
        printf("Ничего не найдено.\n");
    else
        printf("Найдено записей: %zu\n", found);

    free(query);
}

static void print_menu(void)
{
    printf("\n===== Password keeper (стек) =====\n");
    printf(" 1. push   - добавить запись\n");
    printf(" 2. pop    - удалить верхнюю запись\n");
    printf(" 3. peek   - показать верхнюю запись\n");
    printf(" 4. list   - показать все записи\n");
    printf(" 5. search - найти запись по сервису\n");
    printf(" 6. size   - количество записей\n");
    printf(" 0. выход\n");
}

/* ---------- main ---------- */

int main(void)
{
    Stack stack;
    stack_init(&stack);

    for (;;) {
        print_menu();

        char *line = read_line("Выбор: ");
        if (line == NULL)           /* EOF (Ctrl+D) */
            break;

        char *end;
        long choice = strtol(line, &end, 10);
        int valid = (line[0] != '\0' && *end == '\0');
        free(line);

        if (!valid) {
            printf("Введите число от 0 до 6.\n");
            continue;
        }

        switch (choice) {
        case 1: action_push(&stack);   break;
        case 2: action_pop(&stack);    break;
        case 3: action_peek(&stack);   break;
        case 4: action_list(&stack);   break;
        case 5: action_search(&stack); break;
        case 6: printf("Записей в стеке: %zu\n", stack.size); break;
        case 0:
            stack_destroy(&stack);
            printf("Выход.\n");
            return EXIT_SUCCESS;
        default:
            printf("Нет такого пункта меню.\n");
        }
    }

    stack_destroy(&stack);
    return EXIT_SUCCESS;
}
