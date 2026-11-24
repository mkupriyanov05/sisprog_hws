/* prog_06_02.c - именованный канал (FIFO) + перехват всех сигналов.
 *
 * ЧТО ДЕЛАЕТ ПРОГРАММА
 *   Передаёт текст между двумя НЕЗАВИСИМЫМИ процессами (в разных консолях)
 *   через специальный файл - именованный канал (FIFO).
 *
 *     консоль 1:  ./prog_06_02 r /tmp/my_fifo      <- читатель
 *     консоль 2:  ./prog_06_02 w /tmp/my_fifo      <- писатель
 *
 *   Что вы вводите в консоли писателя, то появляется в консоли читателя.
 *
 * ЧЕМ ОТЛИЧАЕТСЯ ОТ prog_06_01
 *   У FIFO есть ИМЯ в файловой системе (ls -l покажет тип 'p'), поэтому его
 *   могут открыть процессы, никак не связанные родством. Создаёт его вызов
 *   mkfifo(); права 0666 урезаются umask. Дальше с ним работают обычными
 *   open/read/write/close. Данные хранятся в памяти ядра, на диск не пишутся.
 *
 * БЛОКИРОВКА ПРИ open()
 *   open(O_RDONLY) ждёт, пока кто-нибудь откроет FIFO на запись, и наоборот.
 *   Поэтому каждая сторона пишет "жду ..." и стоит, пока не запустят вторую.
 *   Сигнал прерывает это ожидание (open вернёт EINTR) - см. open_fifo().
 *
 * ЗАВЕРШЕНИЕ
 *   Писатель: Ctrl+D (или сигнал) - закрывает канал.
 *   Читатель: видит EOF (read вернул 0), печатает об этом, удаляет файл FIFO
 *   (unlink) и выходит. Если писатель выйдет первым - читатель завершится.
 *   Если читатель уйдёт раньше - write у писателя вернёт EPIPE.
 *
 * СИГНАЛЫ
 *   Обе стороны перехватывают ВСЕ сигналы (кроме SIGKILL/SIGSTOP) и печатают
 *   их. SIGINT, SIGTERM, SIGHUP и SIGQUIT вызывают аккуратный выход (читатель
 *   при этом удаляет FIFO), остальные только выводятся. SIGPIPE тоже
 *   перехвачен, поэтому запись в закрытый канал не убивает процесс, а даёт
 *   ошибку EPIPE.
 *
 * СБОРКА:  make -f makefile_06_02.mk
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

/* open() на FIFO блокируется до появления второй стороны. Если во время
 * ожидания пришёл сигнал, open() вернёт -1/EINTR (SA_RESTART не ставили).
 * Тогда разбираем сигнал: запрос на завершение -> выходим, иначе ждём дальше.
 * (Если сигнал придёт в микроскопическое окно между проверкой и входом в
 * open(), ожидание не прервётся - тогда достаточно послать сигнал ещё раз.) */
static int open_fifo(const char *path, int flags, int sigfd, const char *who)
{
    for (;;) {
        int fd = open(path, flags);
        if (fd != -1)
            return fd;
        if (errno != EINTR) {
            perror("open");
            return -1;
        }
        if (sig_report(sigfd, who)) {
            printf("[%s] получен сигнал завершения во время ожидания\n", who);
            return -1;
        }
    }
}

static int write_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        n   -= (size_t)w;
    }
    return 0;
}

static int run_reader(const char *path, int sigfd)
{
    printf("[читатель %d] жду писателя на %s ...\n", getpid(), path);
    fflush(stdout);

    int fd = open_fifo(path, O_RDONLY, sigfd, "читатель");
    if (fd == -1) {
        unlink(path);
        return 1;
    }
    puts("[читатель] писатель подключился");
    fflush(stdout);

    struct pollfd p[2] = { { fd, POLLIN, 0 }, { sigfd, POLLIN, 0 } };
    char buf[256];

    for (;;) {
        if (poll(p, 2, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (p[1].revents & POLLIN) {
            if (sig_report(sigfd, "читатель")) {
                puts("[читатель] получен сигнал завершения");
                break;
            }
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0) { if (errno == EINTR) continue; perror("read"); break; }
            if (n == 0) {
                puts("[читатель] писатель закрыл канал (EOF)");
                break;
            }
            fputs("[читатель] получено: ", stdout);
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
    }
    close(fd);
    unlink(path);                       /* удаляем файл FIFO из файловой системы */
    return 0;
}

static int run_writer(const char *path, int sigfd)
{
    printf("[писатель %d] жду читателя на %s ...\n", getpid(), path);
    fflush(stdout);

    int fd = open_fifo(path, O_WRONLY, sigfd, "писатель");
    if (fd == -1)
        return 1;
    puts("[писатель] читатель подключился. Вводите строки (Ctrl+D - выход):");
    fflush(stdout);

    struct pollfd p[2] = { { STDIN_FILENO, POLLIN, 0 }, { sigfd, POLLIN, 0 } };
    char buf[256];
    int rc = 0;

    for (;;) {
        if (poll(p, 2, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            rc = 1;
            break;
        }
        if (p[1].revents & POLLIN) {
            if (sig_report(sigfd, "писатель")) {
                puts("[писатель] получен сигнал завершения");
                break;
            }
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n < 0) { if (errno == EINTR) continue; perror("read"); rc = 1; break; }
            if (n == 0) break;          /* Ctrl+D */
            if (write_all(fd, buf, (size_t)n) == -1) {
                perror("[писатель] write");     /* EPIPE: читатель ушёл */
                rc = 1;
                break;
            }
        }
    }
    close(fd);                          /* читатель увидит EOF */
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "r") != 0 && strcmp(argv[1], "w") != 0)) {
        fprintf(stderr, "Использование: %s r|w <путь_к_fifo>\n", argv[0]);
        return 1;
    }

    int sigfd = sig_init();
    if (sigfd == -1) { perror("sig_init"); return 1; }

    /* Создаём FIFO. Если он уже существует (например, его создал вторая
     * сторона) - это не ошибка. */
    if (mkfifo(argv[2], 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        return 1;
    }

    return argv[1][0] == 'r' ? run_reader(argv[2], sigfd)
                             : run_writer(argv[2], sigfd);
}
