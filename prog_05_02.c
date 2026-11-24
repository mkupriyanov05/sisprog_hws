/*
 * prog_05_02.c
 *
 * Password keeper на основе двусвязного списка.
 *  - список НЕ на массиве: каждая запись - отдельный узел в куче,
 *    узлы связаны указателями prev / next
 *  - динамическая память: malloc() / free()
 *  - cli-меню: add / rm / edit / prev / next / show / list / search / size
 *  - в списке есть "текущая запись" (курсор): по ней работают
 *    rm, edit, show, а prev / next передвигают курсор
 *  - search: просмотр конкретной записи по названию сервиса
 *
 * Сборка:  make -f makefile_05_02.mk
 * Запуск:  ./prog_05_02
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

/* ---------- Структуры ---------- */

/* Узел списка: одна запись + указатели на соседей.
 * Все три строки и сам узел лежат в куче (malloc). */
typedef struct Node {
    char *service;
    char *login;
    char *password;
    struct Node *prev;   /* предыдущий узел (NULL у первого) */
    struct Node *next;   /* следующий узел (NULL у последнего) */
} Node;

/* Двусвязный список с курсором. */
typedef struct {
    Node  *head;      /* первый узел */
    Node  *tail;      /* последний узел */
    Node  *current;   /* текущая запись (курсор), NULL если список пуст */
    size_t size;      /* количество узлов */
} List;

/* ---------- Вспомогательные функции ---------- */

/*
 * xmalloc - обёртка над malloc().
 * Выделяет n байт; если память не выделилась, печатает ошибку в stderr
 * и завершает программу. Благодаря этому после вызова не нужно
 * проверять результат на NULL.
 */
static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        fprintf(stderr, "Ошибка: не хватает памяти\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

/*
 * copy_str - копия строки в куче (аналог strdup()).
 * Выделяет strlen(s) + 1 байт через xmalloc и копирует туда строку
 * вместе с '\0'. Результат нужно освободить через free().
 */
static char *copy_str(const char *s)
{
    size_t len = strlen(s);
    char *p = xmalloc(len + 1);
    memcpy(p, s, len + 1);
    return p;
}

/*
 * read_line - читает одну строку из stdin.
 * Печатает приглашение prompt (если оно не NULL), читает строку через
 * fgets() в буфер 512 байт, убирает символ '\n'. Если строка длиннее буфера,
 * остаток вычищается из stdin, чтобы не попасть в следующий ввод.
 * Возвращает копию строки в куче (её нужно освободить через free())
 * или NULL, если ввод закончился (EOF, Ctrl+D).
 */
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

/*
 * read_password - ввод пароля без отображения символов на экране.
 * Если stdin - терминал, через termios временно выключает флаг ECHO,
 * читает строку функцией read_line(), затем возвращает настройки терминала
 * и печатает перевод строки (его не напечатал сам пользователь).
 * Если stdin - не терминал (pipe, файл), просто читает строку.
 * Возвращает строку в куче или NULL при EOF.
 */
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

/* ---------- Операции списка ---------- */

/*
 * list_init - создаёт пустой список (все указатели NULL, size = 0).
 */
static void list_init(List *l)
{
    l->head = NULL;
    l->tail = NULL;
    l->current = NULL;
    l->size = 0;
}

/*
 * list_is_empty - возвращает 1, если в списке нет узлов, иначе 0.
 */
static int list_is_empty(const List *l)
{
    return l->size == 0;
}

/*
 * node_create - создаёт новый узел в куче.
 * Узел забирает владение строками service, login и password
 * (они должны быть выделены через malloc). Указатели prev и next = NULL.
 */
static Node *node_create(char *service, char *login, char *password)
{
    Node *n = xmalloc(sizeof(Node));
    n->service = service;
    n->login = login;
    n->password = password;
    n->prev = NULL;
    n->next = NULL;
    return n;
}

/*
 * node_free - освобождает узел целиком: три строки и сам узел.
 * Из списка узел НЕ убирает (для этого есть list_remove).
 */
static void node_free(Node *n)
{
    free(n->service);
    free(n->login);
    free(n->password);
    free(n);
}

/*
 * list_append - добавляет узел в конец списка.
 * Правит указатели tail->next и n->prev, увеличивает size.
 * Курсор переносится на добавленный узел.
 */
static void list_append(List *l, Node *n)
{
    n->prev = l->tail;
    n->next = NULL;

    if (l->tail != NULL)
        l->tail->next = n;
    else
        l->head = n;          /* список был пуст */
    l->tail = n;

    l->current = n;
    l->size++;
}

/*
 * list_remove - вынимает узел из списка и освобождает его память.
 * Соседние узлы связываются друг с другом, при необходимости обновляются
 * head и tail. Если удаляется текущая запись, курсор переходит
 * на следующую, а если её нет - на предыдущую (или в NULL, если
 * список опустел).
 */
static void list_remove(List *l, Node *n)
{
    if (n->prev != NULL)
        n->prev->next = n->next;
    else
        l->head = n->next;

    if (n->next != NULL)
        n->next->prev = n->prev;
    else
        l->tail = n->prev;

    if (l->current == n)
        l->current = (n->next != NULL) ? n->next : n->prev;

    l->size--;
    node_free(n);
}

/*
 * list_next - передвигает курсор на следующую запись.
 * Возвращает 1, если сдвинулись, и 0, если запись последняя (или список пуст).
 */
static int list_next(List *l)
{
    if (l->current == NULL || l->current->next == NULL)
        return 0;
    l->current = l->current->next;
    return 1;
}

/*
 * list_prev - передвигает курсор на предыдущую запись.
 * Возвращает 1, если сдвинулись, и 0, если запись первая (или список пуст).
 */
static int list_prev(List *l)
{
    if (l->current == NULL || l->current->prev == NULL)
        return 0;
    l->current = l->current->prev;
    return 1;
}

/*
 * list_position - порядковый номер узла в списке (с единицы).
 * Проходит от head до n, считая шаги.
 */
static size_t list_position(const List *l, const Node *n)
{
    size_t pos = 1;
    for (const Node *p = l->head; p != NULL && p != n; p = p->next)
        pos++;
    return pos;
}

/*
 * list_destroy - полностью уничтожает список.
 * Проходит по всем узлам и освобождает каждый (node_free),
 * не забывая взять next до освобождения узла.
 */
static void list_destroy(List *l)
{
    Node *p = l->head;
    while (p != NULL) {
        Node *next = p->next;
        node_free(p);
        p = next;
    }
    list_init(l);
}

/* ---------- Вывод ---------- */

/*
 * print_node - печатает одну запись.
 * pos - порядковый номер записи, is_current - ставить ли маркер ">"
 * (так видно, где стоит курсор). Если show_password != 0, пароль выводится
 * как есть, иначе вместо него печатается "********".
 */
static void print_node(size_t pos, const Node *n, int is_current, int show_password)
{
    printf(" %c[%zu] Сервис: %s\n", is_current ? '>' : ' ', pos, n->service);
    printf("      Логин:  %s\n", n->login);
    if (show_password)
        printf("      Пароль: %s\n", n->password);
    else
        printf("      Пароль: ********\n");
}

/* ---------- Действия меню ---------- */

/*
 * action_add - пункт меню "add".
 * Запрашивает сервис, логин и пароль (пароль без эха) и добавляет
 * запись в конец списка. Если ввод прервали (EOF) или название сервиса
 * пустое, запись не добавляется, а выделенная память освобождается.
 */
static void action_add(List *l)
{
    char *service  = read_line("Сервис:  ");
    char *login    = read_line("Логин:   ");
    char *password = read_password("Пароль:  ");

    if (service == NULL || login == NULL || password == NULL) {
        free(service);
        free(login);
        free(password);
        printf("Ввод прерван, запись не добавлена.\n");
        return;
    }
    if (service[0] == '\0') {
        free(service);
        free(login);
        free(password);
        printf("Название сервиса не может быть пустым.\n");
        return;
    }

    list_append(l, node_create(service, login, password));
    printf("Запись добавлена (всего: %zu).\n", l->size);
}

/*
 * action_rm - пункт меню "rm".
 * Удаляет текущую запись и освобождает её память.
 * Курсор переходит на соседнюю запись.
 */
static void action_rm(List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }
    printf("Удаляется запись:\n");
    print_node(list_position(l, l->current), l->current, 1, 0);
    list_remove(l, l->current);
    printf("Готово (осталось: %zu).\n", l->size);
}

/*
 * edit_field - заменяет одно текстовое поле записи.
 * Печатает приглашение с подсказкой "Enter - оставить без изменений"
 * (если current_value != NULL, показывает и старое значение),
 * читает новое значение (hidden != 0 - без эха, для пароля).
 * Пустой ввод или EOF оставляют поле как есть; иначе старая строка
 * освобождается (free), а в поле записывается новая.
 */
static void edit_field(char **field, const char *label,
                       const char *current_value, int hidden)
{
    char *input;

    if (current_value != NULL)
        printf("%s [%s] (Enter - оставить): ", label, current_value);
    else
        printf("%s (Enter - оставить): ", label);
    fflush(stdout);

    input = hidden ? read_password(NULL) : read_line(NULL);
    if (input == NULL || input[0] == '\0') {
        free(input);
        return;
    }
    free(*field);
    *field = input;
}

/*
 * action_edit - пункт меню "edit".
 * Позволяет изменить сервис, логин и пароль текущей записи.
 * Поля, в которых просто нажали Enter, не меняются.
 */
static void action_edit(List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }
    Node *n = l->current;

    printf("Редактирование записи:\n");
    print_node(list_position(l, n), n, 1, 0);

    edit_field(&n->service, "Сервис", n->service, 0);
    edit_field(&n->login, "Логин", n->login, 0);
    edit_field(&n->password, "Новый пароль", NULL, 1);

    printf("Запись обновлена.\n");
}

/*
 * action_next - пункт меню "next".
 * Переходит к следующей записи и показывает её (пароль скрыт).
 */
static void action_next(List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }
    if (!list_next(l)) {
        printf("Это последняя запись.\n");
        return;
    }
    print_node(list_position(l, l->current), l->current, 1, 0);
}

/*
 * action_prev - пункт меню "prev".
 * Переходит к предыдущей записи и показывает её (пароль скрыт).
 */
static void action_prev(List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }
    if (!list_prev(l)) {
        printf("Это первая запись.\n");
        return;
    }
    print_node(list_position(l, l->current), l->current, 1, 0);
}

/*
 * action_show - пункт меню "show".
 * Показывает текущую запись вместе с паролем.
 */
static void action_show(const List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }
    print_node(list_position(l, l->current), l->current, 1, 1);
}

/*
 * action_list - пункт меню "list".
 * Печатает все записи от первой к последней. Пароли скрыты,
 * текущая запись помечена ">".
 */
static void action_list(const List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }
    printf("Записи (пароли скрыты, > - текущая):\n");

    size_t pos = 1;
    for (const Node *p = l->head; p != NULL; p = p->next, pos++)
        print_node(pos, p, p == l->current, 0);
}

/*
 * action_search - пункт меню "search".
 * Запрашивает строку и ищет записи, в названии сервиса которых она
 * встречается как подстрока (без учёта регистра, strcasestr).
 * Найденные записи печатаются вместе с паролем, а курсор
 * ставится на первую найденную.
 */
static void action_search(List *l)
{
    if (list_is_empty(l)) {
        printf("Список пуст.\n");
        return;
    }

    char *query = read_line("Название сервиса для поиска: ");
    if (query == NULL || query[0] == '\0') {
        free(query);
        printf("Пустой запрос.\n");
        return;
    }

    size_t found = 0;
    size_t pos = 1;
    for (Node *p = l->head; p != NULL; p = p->next, pos++) {
        if (strcasestr(p->service, query) != NULL) {
            if (found == 0)
                l->current = p;
            print_node(pos, p, p == l->current, 1);
            found++;
        }
    }
    if (found == 0)
        printf("Ничего не найдено.\n");
    else
        printf("Найдено записей: %zu (курсор на первой найденной)\n", found);

    free(query);
}

/*
 * print_menu - печатает список пунктов меню.
 */
static void print_menu(void)
{
    printf("\n===== Password keeper (список) =====\n");
    printf(" 1. add    - добавить запись в конец\n");
    printf(" 2. rm     - удалить текущую запись\n");
    printf(" 3. edit   - изменить текущую запись\n");
    printf(" 4. next   - следующая запись\n");
    printf(" 5. prev   - предыдущая запись\n");
    printf(" 6. show   - показать текущую запись (с паролем)\n");
    printf(" 7. list   - показать все записи\n");
    printf(" 8. search - найти запись по сервису\n");
    printf(" 9. size   - количество записей\n");
    printf(" 0. выход\n");
}

/* ---------- main ---------- */

/*
 * main - точка входа.
 * Создаёт список и в цикле показывает меню, читает номер пункта (strtol с
 * проверкой, что введено именно число) и вызывает нужное действие.
 * Выход по пункту 0 или по EOF (Ctrl+D); в обоих случаях перед
 * завершением вызывается list_destroy(), чтобы не было утечек памяти.
 */
int main(void)
{
    List list;
    list_init(&list);

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
            printf("Введите число от 0 до 9.\n");
            continue;
        }

        switch (choice) {
        case 1: action_add(&list);    break;
        case 2: action_rm(&list);     break;
        case 3: action_edit(&list);   break;
        case 4: action_next(&list);   break;
        case 5: action_prev(&list);   break;
        case 6: action_show(&list);   break;
        case 7: action_list(&list);   break;
        case 8: action_search(&list); break;
        case 9: printf("Записей в списке: %zu\n", list.size); break;
        case 0:
            list_destroy(&list);
            printf("Выход.\n");
            return EXIT_SUCCESS;
        default:
            printf("Нет такого пункта меню.\n");
        }
    }

    list_destroy(&list);
    return EXIT_SUCCESS;
}
