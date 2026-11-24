/*
 * prog_05_03.c
 *
 * Сортировка кучей (heap sort).
 *
 * Использование:
 *   prog_05_03 [--max | --min] -f /path/to/file
 *   prog_05_03 [--max | --min] "5 3 8 1"
 *
 *   -f FILE   взять числа из файла
 *   STRING    или передать числа строкой прямо в аргументах
 *   --min     сначала самые маленькие (по возрастанию) - режим по умолчанию
 *   --max     сначала самые большие (по убыванию)
 *   -h, --help  справка
 *
 * Числа можно разделять пробелами, переводами строк, запятыми или ';'.
 * Допускаются целые и дробные числа (например: 3, -7, 2.5, 1e3).
 *
 * Динамическая память: malloc() / free().
 *
 * Сборка:  make -f makefile_05_03.mk
 * Запуск:  ./prog_05_03 --max "5 3 8 1"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#define INITIAL_CAPACITY 16

/* ---------- Типы ---------- */

/* Режим сортировки. */
typedef enum {
    MODE_MIN,   /* --min: по возрастанию (минимум первым) */
    MODE_MAX    /* --max: по убыванию (максимум первым) */
} Mode;

/* Динамический массив чисел. */
typedef struct {
    double *data;      /* числа в куче */
    size_t  size;      /* сколько чисел сейчас */
    size_t  capacity;  /* сколько влезает без расширения */
} Array;

/* ---------- Работа с памятью ---------- */

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
 * array_init - создаёт пустой массив чисел на INITIAL_CAPACITY элементов.
 */
static void array_init(Array *a)
{
    a->capacity = INITIAL_CAPACITY;
    a->size = 0;
    a->data = xmalloc(a->capacity * sizeof(double));
}

/*
 * array_push - добавляет число в конец массива.
 * Если место закончилось, массив расширяется вдвое:
 * malloc нового, memcpy, free старого.
 */
static void array_push(Array *a, double value)
{
    if (a->size == a->capacity) {
        size_t new_cap = a->capacity * 2;
        double *new_data = xmalloc(new_cap * sizeof(double));

        memcpy(new_data, a->data, a->size * sizeof(double));
        free(a->data);

        a->data = new_data;
        a->capacity = new_cap;
    }
    a->data[a->size++] = value;
}

/*
 * array_free - освобождает память массива и обнуляет поля.
 */
static void array_free(Array *a)
{
    free(a->data);
    a->data = NULL;
    a->size = a->capacity = 0;
}

/* ---------- Heap sort ---------- */

/*
 * swap - меняет местами два числа.
 */
static void swap(double *x, double *y)
{
    double t = *x;
    *x = *y;
    *y = t;
}

/*
 * goes_above - "должен ли x находиться выше y в куче".
 * Для max-кучи это x > y, для min-кучи это x < y.
 */
static int goes_above(double x, double y, int max_heap)
{
    return max_heap ? (x > y) : (x < y);
}

/*
 * sift_down - просеивание вниз (восстановление свойства кучи).
 * Куча лежит в массиве a[0..n-1]: потомки элемента i - это 2*i+1 и 2*i+2.
 * Берёт элемент i и, пока он не на своём месте, меняет его с тем из
 * потомков, который должен быть выше (больший для max-кучи, меньший
 * для min-кучи). Сложность - O(log n).
 */
static void sift_down(double *a, size_t n, size_t i, int max_heap)
{
    for (;;) {
        size_t left  = 2 * i + 1;
        size_t right = left + 1;
        size_t top   = i;

        if (left < n && goes_above(a[left], a[top], max_heap))
            top = left;
        if (right < n && goes_above(a[right], a[top], max_heap))
            top = right;

        if (top == i)
            break;

        swap(&a[i], &a[top]);
        i = top;
    }
}

/*
 * build_heap - превращает произвольный массив в кучу.
 * Вызывает sift_down для всех внутренних узлов, от последнего к корню
 * (листья - уже готовые кучи из одного элемента). Сложность - O(n).
 */
static void build_heap(double *a, size_t n, int max_heap)
{
    for (size_t i = n / 2; i > 0; i--)
        sift_down(a, n, i - 1, max_heap);
}

/*
 * heap_sort - сортировка кучей на месте, O(n log n).
 * 1) строит кучу из всего массива;
 * 2) повторяет: меняет корень (экстремум) с последним элементом кучи,
 *    уменьшает размер кучи на 1 и просеивает новый корень вниз.
 * Извлечённые элементы накапливаются в конце массива, поэтому
 * max-куча даёт порядок по возрастанию, а min-куча - по убыванию:
 *   MODE_MIN (по возрастанию) -> max-куча
 *   MODE_MAX (по убыванию)    -> min-куча
 */
static void heap_sort(double *a, size_t n, Mode mode)
{
    int max_heap = (mode == MODE_MIN);

    if (n < 2)
        return;

    build_heap(a, n, max_heap);

    for (size_t end = n - 1; end > 0; end--) {
        swap(&a[0], &a[end]);
        sift_down(a, end, 0, max_heap);
    }
}

/* ---------- Ввод ---------- */

/*
 * read_file - читает файл целиком в строку в куче.
 * Буфер растёт вдвое (malloc + memcpy + free), в конце добавляется '\0'.
 * Возвращает строку (её нужно освободить через free()) или NULL при ошибке
 * (сообщение об ошибке уже напечатано).
 */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "Ошибка: не удалось открыть '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }

    size_t cap = 1024, len = 0, n;
    char *buf = xmalloc(cap);

    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {                 /* буфер заполнен - расширяем */
            char *bigger = xmalloc(cap * 2);
            memcpy(bigger, buf, len);
            free(buf);
            buf = bigger;
            cap *= 2;
        }
    }

    if (ferror(f)) {
        fprintf(stderr, "Ошибка чтения файла '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    buf[len] = '\0';
    fclose(f);
    return buf;
}

/*
 * is_separator - разделитель между числами:
 * пробельный символ (пробел, таб, перевод строки), запятая или ';'.
 */
static int is_separator(char c)
{
    return isspace((unsigned char)c) || c == ',' || c == ';';
}

/*
 * parse_numbers - разбирает строку на числа и добавляет их в массив.
 * Каждое число читается через strtod(). Если токен не является числом
 * (например "12abc", "abc", "nan", слишком большое значение), печатает
 * ошибку с этим токеном и возвращает -1. При успехе возвращает 0.
 */
static int parse_numbers(const char *text, Array *arr)
{
    const char *p = text;

    while (*p != '\0') {
        if (is_separator(*p)) {
            p++;
            continue;
        }

        char *end;
        double value = strtod(p, &end);

        if (end == p || (*end != '\0' && !is_separator(*end)) || !isfinite(value)) {
            int len = (int)strcspn(p, " \t\n\r\v\f,;");
            fprintf(stderr, "Ошибка: '%.*s' - не число\n", len, p);
            return -1;
        }

        array_push(arr, value);
        p = end;
    }
    return 0;
}

/* ---------- Вывод ---------- */

/*
 * print_usage - печатает краткую справку по использованию.
 * stream - куда писать (stdout для --help, stderr при ошибке).
 */
static void print_usage(FILE *stream, const char *prog)
{
    fprintf(stream,
            "Использование:\n"
            "  %s [--max | --min] -f /path/to/file\n"
            "  %s [--max | --min] \"5 3 8 1\"\n"
            "\n"
            "  -f FILE     взять числа из файла\n"
            "  --min       по возрастанию (по умолчанию)\n"
            "  --max       по убыванию\n"
            "  -h, --help  эта справка\n"
            "\n"
            "Числа разделяются пробелами, переводами строк, ',' или ';'.\n",
            prog, prog);
}

/*
 * print_array - печатает числа через пробел в одну строку.
 * Формат %.15g: целые выводятся без хвоста ".0", дробные - без мусора.
 */
static void print_array(const Array *a)
{
    for (size_t i = 0; i < a->size; i++)
        printf("%s%.15g", i ? " " : "", a->data[i]);
    printf("\n");
}

/*
 * looks_like_option - похож ли аргумент на ключ командной строки.
 * Ключ начинается с '-', а дальше идёт не цифра и не точка,
 * поэтому отрицательные числа ("-5", "-.5") остаются данными.
 */
static int looks_like_option(const char *arg)
{
    return arg[0] == '-' && arg[1] != '\0'
        && !isdigit((unsigned char)arg[1]) && arg[1] != '.';
}

/* ---------- main ---------- */

/*
 * main - точка входа.
 * Разбирает аргументы (--max / --min, -f FILE, -h), числа из строковых
 * аргументов сразу добавляет в массив. Если указан -f, читает и разбирает
 * файл. Затем сортирует массив кучей и печатает результат.
 * Коды возврата: 0 - успех, 1 - ошибка (неверные аргументы, файл, ввод).
 */
int main(int argc, char *argv[])
{
    Mode mode = MODE_MIN;
    int mode_set = 0;
    const char *file_path = NULL;
    int have_text = 0;
    int status = EXIT_FAILURE;

    Array arr;
    array_init(&arr);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout, argv[0]);
            array_free(&arr);
            return EXIT_SUCCESS;
        } else if (strcmp(arg, "--max") == 0 || strcmp(arg, "--min") == 0) {
            Mode requested = (arg[2] == 'm' && arg[3] == 'a') ? MODE_MAX : MODE_MIN;
            if (mode_set && requested != mode) {
                fprintf(stderr, "Ошибка: --max и --min нельзя указывать вместе\n");
                goto cleanup;
            }
            mode = requested;
            mode_set = 1;
        } else if (strcmp(arg, "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Ошибка: после -f нужен путь к файлу\n");
                goto cleanup;
            }
            if (file_path != NULL) {
                fprintf(stderr, "Ошибка: -f указан больше одного раза\n");
                goto cleanup;
            }
            file_path = argv[++i];
        } else if (looks_like_option(arg)) {
            fprintf(stderr, "Ошибка: неизвестный ключ '%s'\n", arg);
            print_usage(stderr, argv[0]);
            goto cleanup;
        } else {
            have_text = 1;
            if (parse_numbers(arg, &arr) != 0)
                goto cleanup;
        }
    }

    if (file_path != NULL && have_text) {
        fprintf(stderr, "Ошибка: нужно что-то одно - файл (-f) или строка с числами\n");
        goto cleanup;
    }
    if (file_path == NULL && !have_text) {
        print_usage(stderr, argv[0]);
        goto cleanup;
    }

    if (file_path != NULL) {
        char *text = read_file(file_path);
        if (text == NULL)
            goto cleanup;

        int rc = parse_numbers(text, &arr);
        free(text);
        if (rc != 0)
            goto cleanup;
    }

    if (arr.size == 0) {
        fprintf(stderr, "Ошибка: во входных данных нет чисел\n");
        goto cleanup;
    }

    heap_sort(arr.data, arr.size, mode);
    print_array(&arr);
    status = EXIT_SUCCESS;

cleanup:
    array_free(&arr);
    return status;
}
