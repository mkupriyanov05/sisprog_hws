/* prog_06_01.c - неименованный канал (pipe) + перехват всех сигналов.
 *
 * ЧТО ДЕЛАЕТ ПРОГРАММА
 *   Родитель создаёт потомка через fork() и общается с ним по двум
 *   неименованным каналам:
 *
 *       родитель --- to_child  ---> потомок
 *       родитель <-- to_parent ---- потомок
 *
 *   Вы вводите строку -> родитель пишет её в to_child -> потомок читает,
 *   переводит в ВЕРХНИЙ РЕГИСТР и пишет в to_parent -> родитель печатает ответ.
 *
 * ПОЧЕМУ ДВА КАНАЛА
 *   pipe() даёт канал в одну сторону: из fd[1] можно только писать, из fd[0]
 *   только читать. Для диалога в обе стороны нужна пара каналов.
 *
 * ПОЧЕМУ ТОЛЬКО РОДСТВЕННЫЕ ПРОЦЕССЫ
 *   У неименованного канала нет имени в файловой системе. Единственный способ
 *   получить к нему доступ - унаследовать открытые дескрипторы при fork().
 *   Поэтому pipe() вызывается ДО fork().
 *
 * ПОЧЕМУ ЛИШНИЕ КОНЦЫ КАНАЛОВ ЗАКРЫВАЮТСЯ
 *   Читатель получает EOF (read вернёт 0) только тогда, когда закрыты ВСЕ
 *   дескрипторы записи этого канала. Если потомок оставит открытым свой
 *   экземпляр to_child[1], он никогда не дождётся EOF и не завершится.
 *
 * ЗАВЕРШЕНИЕ
 *   Ctrl+D (конец ввода): родитель закрывает to_child[1], потомок видит EOF,
 *   закрывает to_parent[1] и выходит; родитель видит EOF на to_parent[0],
 *   делает waitpid() и тоже выходит. Ответ потомка не теряется.
 *
 * СИГНАЛЫ
 *   И родитель, и потомок перехватывают ВСЕ сигналы (кроме SIGKILL/SIGSTOP)
 *   и печатают их с пометкой "родитель" / "потомок". SIGINT, SIGTERM, SIGHUP
 *   и SIGQUIT приводят к аккуратному завершению, остальные только
 *   выводятся. Ctrl+C терминал шлёт ВСЕМ процессам группы - вы увидите
 *   сообщения от обоих.
 *
 * ПРОВЕРКА (в другой консоли; pid печатается при запуске)
 *   kill -USR1 <pid>   kill -HUP <pid>   kill -34 <pid>   kill -TERM <pid>
 *
 * СБОРКА:  make -f makefile_06_01.mk        ЗАПУСК:  ./prog_06_01
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ================================================================== */
/*  ПЕРЕХВАТ ВСЕХ СИГНАЛОВ                                            */
/*  (этот блок одинаков во всех трёх программах - отдельных            */
/*  header-файлов нет, поэтому он продублирован в каждом .c)           */
/*                                                                    */
/*  Как это работает ("self-pipe trick"):                             */
/*   1. sig_init() вешает sigaction() на каждый сигнал 1..NSIG-1.      */
/*      SIGKILL и SIGSTOP перехватить нельзя - ядро запрещает это.     */
/*      Сигналы 32 и 33 занимает сама glibc (потоки), sigaction на     */
/*      них вернёт EINVAL - они просто пропускаются.                   */
/*   2. Обработчик sig_handler() ничего не печатает: printf() внутри   */
/*      обработчика небезопасен. Он делает единственное безопасное     */
/*      действие - write() номера сигнала и pid отправителя в          */
/*      служебный pipe.                                                */
/*   3. Читающий конец этого pipe лежит в poll() основного цикла       */
/*      рядом с остальными дескрипторами. Когда он становится готов,   */
/*      sig_report() читает события и уже в обычном коде печатает их.  */
/*   4. SA_RESTART НЕ ставится: блокирующий вызов (например open() на  */
/*      FIFO) прерывается с EINTR, и программа успевает среагировать.  */
/* ================================================================== */

struct sigmsg {
    int   signo;
    pid_t pid;
};

static int sig_pipe[2] = { -1, -1 };

static void sig_handler(int signo, siginfo_t *si, void *ctx)
{
    (void)ctx;
    int saved_errno = errno;            /* обработчик не должен портить errno */

    /* Аппаратные ошибки (SIGSEGV, SIGBUS, SIGFPE, SIGILL с si_code > 0):
     * если просто вернуться из обработчика, процессор повторит ту же
     * инструкцию и программа зациклится. Поэтому тут завершаемся.
     * Такой же сигнал, посланный командой kill, имеет si_code <= 0 и
     * обрабатывается как обычный. */
    if ((signo == SIGSEGV || signo == SIGBUS || signo == SIGFPE ||
         signo == SIGILL) && si && si->si_code > 0) {
        static const char msg[] = "\n[сигнал] аппаратная ошибка, аварийный выход\n";
        ssize_t r = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void)r;
        _exit(128 + signo);
    }

    struct sigmsg m = { signo, si ? si->si_pid : 0 };
    ssize_t r = write(sig_pipe[1], &m, sizeof m);   /* < PIPE_BUF, значит атомарно */
    (void)r;
    errno = saved_errno;
}

/* Устанавливает перехват. Возвращает fd, который надо положить в poll(),
 * либо -1. Повторный вызов (в потомке после fork) создаёт НОВЫЙ pipe,
 * чтобы потомок не писал сигналы в канал родителя. */
static int sig_init(void)
{
    if (sig_pipe[0] != -1) {
        close(sig_pipe[0]);
        close(sig_pipe[1]);
    }
    if (pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC) == -1)
        return -1;

    for (int s = 1; s < NSIG; s++) {
        if (s == SIGKILL || s == SIGSTOP)
            continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = sig_handler;
        sigfillset(&sa.sa_mask);        /* пока работает обработчик, прочие сигналы отложены */
        sa.sa_flags = SA_SIGINFO;       /* SA_SIGINFO даёт pid отправителя */
        sigaction(s, &sa, NULL);        /* для 32 и 33 вернёт EINVAL - это нормально */
    }
    return sig_pipe[0];
}

/* "SIGINT (2, Interrupt)" */
static const char *sig_name(int signo, char *buf, size_t n)
{
    const char *abbr = NULL;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 32))
    abbr = sigabbrev_np(signo);
#endif
    const char *text = strsignal(signo);
    if (abbr)
        snprintf(buf, n, "SIG%s (%d, %s)", abbr, signo, text ? text : "?");
    else
        snprintf(buf, n, "сигнал %d (%s)", signo, text ? text : "?");
    return buf;
}

/* Печатает все накопившиеся сигналы. who - кто сообщает (текст для вывода).
 * pid=0 значит, что сигнал послало ядро (например, вы нажали Ctrl+C).
 * Возвращает 1, если среди сигналов был запрос на завершение
 * (SIGINT / SIGTERM / SIGHUP / SIGQUIT), иначе 0. Остальные сигналы
 * только фиксируются в выводе, программа продолжает работу. */
static int sig_report(int fd, const char *who)
{
    int quit = 0;
    struct sigmsg m;
    char buf[128];

    while (read(fd, &m, sizeof m) == (ssize_t)sizeof m) {
        fprintf(stderr, "[сигнал] %s: %s, отправитель pid=%d\n",
                who, sig_name(m.signo, buf, sizeof buf), (int)m.pid);
        if (m.signo == SIGINT || m.signo == SIGTERM ||
            m.signo == SIGHUP || m.signo == SIGQUIT)
            quit = 1;
    }
    return quit;
}

/* ================================================================== */
/*  Программа                                                         */
/* ================================================================== */

#define BUFSZ 256

static void write_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");            /* например EPIPE, если читатель ушёл */
            return;
        }
        buf += w;
        n   -= (size_t)w;
    }
}

/* Код потомка: читает из in_fd, переводит в верхний регистр, пишет в out_fd. */
static void child_main(int in_fd, int out_fd)
{
    int sigfd = sig_init();             /* свой собственный перехват сигналов */
    if (sigfd == -1) { perror("sig_init"); _exit(1); }

    printf("[потомок %d] запущен, жду данные от родителя\n", getpid());
    fflush(stdout);

    struct pollfd p[2] = { { in_fd, POLLIN, 0 }, { sigfd, POLLIN, 0 } };
    char buf[BUFSZ];

    for (;;) {
        if (poll(p, 2, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (p[1].revents & POLLIN) {
            if (sig_report(sigfd, "потомок")) {
                puts("[потомок] получен сигнал завершения, выхожу");
                break;
            }
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(in_fd, buf, sizeof buf);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) {               /* все концы записи закрыты -> EOF */
                puts("[потомок] родитель закрыл канал (EOF), выхожу");
                break;
            }
            printf("[потомок %d] получено %zd байт, перевожу в верхний регистр\n",
                   getpid(), n);
            fflush(stdout);
            for (ssize_t i = 0; i < n; i++)
                buf[i] = (char)toupper((unsigned char)buf[i]);
            write_all(out_fd, buf, (size_t)n);
        }
    }
    close(in_fd);
    close(out_fd);                      /* родитель увидит EOF на to_parent[0] */
    fflush(stdout);
    _exit(0);
}

int main(void)
{
    int sigfd = sig_init();
    if (sigfd == -1) { perror("sig_init"); return 1; }

    int to_child[2];                    /* [0] - читать, [1] - писать */
    int to_parent[2];
    if (pipe(to_child) == -1 || pipe(to_parent) == -1) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {                     /* ---------- потомок ---------- */
        close(to_child[1]);             /* потомок не пишет в to_child */
        close(to_parent[0]);            /* и не читает из to_parent */
        child_main(to_child[0], to_parent[1]);
    }

    /* ---------- родитель ---------- */
    close(to_child[0]);                 /* родитель не читает из to_child */
    close(to_parent[1]);                /* и не пишет в to_parent */

    printf("[родитель %d] создан потомок %d\n", getpid(), pid);
    puts("Вводите строки (Ctrl+D - выход). Сигналы можно слать командой kill.");

    struct pollfd p[3] = { { STDIN_FILENO, POLLIN, 0 },
                           { to_parent[0], POLLIN, 0 },
                           { sigfd, POLLIN, 0 } };
    char buf[BUFSZ];

    for (;;) {
        if (poll(p, 3, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (p[2].revents & POLLIN) {
            if (sig_report(sigfd, "родитель")) {
                puts("[родитель] получен сигнал завершения, выхожу");
                break;
            }
        }

        if (p[0].revents & (POLLIN | POLLHUP)) {            /* ввод с клавиатуры */
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                /* Конец ввода: закрываем канал к потомку, чтобы он получил EOF,
                 * но продолжаем крутить цикл, пока не придёт его последний ответ. */
                close(to_child[1]);
                to_child[1] = -1;
                p[0].fd = -1;                               /* poll игнорирует fd < 0 */
            } else {
                write_all(to_child[1], buf, (size_t)n);
            }
        }

        if (p[1].revents & (POLLIN | POLLHUP)) {            /* ответ потомка */
            ssize_t n = read(to_parent[0], buf, sizeof buf - 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                puts("[родитель] потомок закрыл канал (завершился)");
                break;
            }
            buf[n] = '\0';
            printf("[родитель] ответ потомка: %s", buf);
            fflush(stdout);
        }
    }

    if (to_child[1] != -1)
        close(to_child[1]);             /* потомок получит EOF и завершится сам */
    close(to_parent[0]);
    waitpid(pid, NULL, 0);              /* забираем потомка, чтобы не было зомби */
    puts("[родитель] готово");
    return 0;
}
