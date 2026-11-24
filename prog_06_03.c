/* prog_06_03.c - p2p-чат с шифрованием (AES-256-GCM) поверх TCP, UDP или ICMP
 *                 + перехват всех сигналов.
 *
 * ЧТО ДЕЛАЕТ ПРОГРАММА
 *   Две копии программы (в двух консолях, на одной или разных машинах)
 *   обмениваются текстовыми сообщениями. Все сообщения шифруются, поэтому
 *   в wireshark/tshark вместо текста видны случайные байты. Сервера нет:
 *   оба участника равноправны (p2p), различие лишь в том, кто первым
 *   "слушает", а кто "подключается" (для TCP).
 *
 * ВЫБОР ПРОТОКОЛА (-m tcp|udp|icmp; без -m программа спросит сама)
 *   TCP  - socket(SOCK_STREAM). Один пир: bind+listen+accept, другой: connect.
 *          TCP - поток байтов без границ сообщений, поэтому перед каждым
 *          кадром передаётся 2 байта его длины.
 *   UDP  - socket(SOCK_DGRAM). Каждый делает bind на СВОЙ порт и sendto на
 *          порт пира. Одно сообщение = одна датаграмма. Пакеты от чужих
 *          адресов отбрасываются.
 *   ICMP - socket(SOCK_RAW, IPPROTO_ICMP), нужен root (или CAP_NET_RAW).
 *          Сообщение едет в поле данных ICMP Echo Request (как у ping).
 *          Заголовок ICMP и контрольную сумму программа строит вручную,
 *          IP-заголовок добавляет ядро. Входящие пакеты приходят вместе с
 *          IP-заголовком, его длину берём из поля IHL. Эхо-ответы, которые
 *          автоматически шлёт ядро, и собственные пакеты (по случайному
 *          идентификатору) игнорируются.
 *
 * ШИФРОВАНИЕ (OpenSSL, libcrypto)
 *   Из общего пароля (-k или ввод при запуске) получается 256-битный ключ
 *   через PBKDF2-HMAC-SHA256. Каждое сообщение шифруется AES-256-GCM со
 *   случайным 12-байтным nonce. GCM ещё считает 16-байтный тег
 *   аутентичности: при неверном ключе или изменённых по пути данных
 *   расшифровка отвергается и выводится предупреждение.
 *   Кадр одинаков для всех протоколов:
 *       "P2PC" (4) | nonce (12) | тег (16) | шифртекст
 *   Флаг -n отключает шифрование - для сравнения в wireshark.
 *
 * ГЛАВНЫЙ ЦИКЛ
 *   poll() одновременно ждёт: ввод с клавиатуры, данные из сокета и
 *   сигналы. Введённая строка шифруется и уходит пиру; пришедший кадр
 *   расшифровывается и печатается с меткой "пир>".
 *
 * СИГНАЛЫ
 *   Перехватываются все (кроме SIGKILL/SIGSTOP) и выводятся. SIGINT, SIGTERM,
 *   SIGHUP, SIGQUIT - корректный выход (закрытие сокета), остальные только
 *   выводятся (например, SIGPIPE при записи в закрытое соединение не убивает
 *   программу, а даёт ошибку отправки).
 *
 * ЗАПУСК (две консоли)
 *   ./prog_06_03 -m tcp -L 5555                       ./prog_06_03 -m tcp -H 127.0.0.1 -P 5555
 *   ./prog_06_03 -m udp -L 5001 -H 127.0.0.1 -P 5002  ./prog_06_03 -m udp -L 5002 -H 127.0.0.1 -P 5001
 *   sudo ./prog_06_03 -m icmp -H 127.0.0.1            (на обоих концах)
 *   -k пароль   -n без шифрования   -h справка
 *
 * СБОРКА: sudo apt install libssl-dev;  make -f makefile_06_03.mk
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

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

/* ------------------------------------------------------------------ */
/*  Криптография                                                      */
/* ------------------------------------------------------------------ */

#define MAGIC      "P2PC"
#define NONCE_LEN  12
#define TAG_LEN    16
#define HDR_LEN    (4 + NONCE_LEN + TAG_LEN)
#define MAX_MSG    1024
#define MAX_FRAME  (HDR_LEN + MAX_MSG)

static uint8_t g_key[32];
static int     g_crypto = 1;

/* Из пароля получаем 256-битный ключ (PBKDF2-HMAC-SHA256).
 * Соль фиксированная, потому что у двух пиров нет другого способа
 * договориться о ней заранее - оба должны получить один и тот же ключ. */
static int derive_key(const char *pass)
{
    static const unsigned char salt[] = "p2p-chat-salt-v1";
    return PKCS5_PBKDF2_HMAC(pass, (int)strlen(pass), salt, sizeof salt - 1,
                             200000, EVP_sha256(), sizeof g_key, g_key) == 1 ? 0 : -1;
}

/* plaintext -> кадр. Возвращает длину кадра или -1. */
static int seal(const uint8_t *pt, int ptlen, uint8_t *frame)
{
    uint8_t *nonce = frame + 4;
    uint8_t *tag   = frame + 4 + NONCE_LEN;
    uint8_t *ct    = frame + HDR_LEN;

    memcpy(frame, MAGIC, 4);

    if (!g_crypto) {                        /* режим -n: без шифрования */
        memset(nonce, 0, NONCE_LEN);
        memset(tag, 0, TAG_LEN);
        memcpy(ct, pt, (size_t)ptlen);
        return HDR_LEN + ptlen;
    }

    if (RAND_bytes(nonce, NONCE_LEN) != 1)  /* новый случайный nonce на КАЖДОЕ сообщение */
        return -1;

    int len = 0, total = 0, ok = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, NONCE_LEN, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL, g_key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, ct, &len, pt, ptlen) == 1) {
        total = len;
        if (EVP_EncryptFinal_ex(ctx, ct + total, &len) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, tag) == 1) {
            total += len;
            ok = 1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok ? HDR_LEN + total : -1;
}

/* кадр -> plaintext. Возвращает длину текста или -1 (чужой кадр / неверный
 * ключ / данные изменены по дороге - GCM это проверяет). */
static int unseal(const uint8_t *frame, int flen, uint8_t *pt)
{
    if (flen < HDR_LEN || memcmp(frame, MAGIC, 4) != 0)
        return -1;

    const uint8_t *nonce = frame + 4;
    const uint8_t *ct    = frame + HDR_LEN;
    int ctlen = flen - HDR_LEN;

    if (!g_crypto) {
        memcpy(pt, ct, (size_t)ctlen);
        return ctlen;
    }

    uint8_t tag[TAG_LEN];
    memcpy(tag, frame + 4 + NONCE_LEN, TAG_LEN);

    int len = 0, total = 0, ok = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, NONCE_LEN, NULL) == 1 &&
        EVP_DecryptInit_ex(ctx, NULL, NULL, g_key, nonce) == 1 &&
        EVP_DecryptUpdate(ctx, pt, &len, ct, ctlen) == 1) {
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, TAG_LEN, tag) == 1 &&
            EVP_DecryptFinal_ex(ctx, pt + total, &len) > 0) {   /* >0 = тег совпал */
            total += len;
            ok = 1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok ? total : -1;
}

/* ------------------------------------------------------------------ */
/*  Сеть: TCP / UDP / ICMP                                            */
/* ------------------------------------------------------------------ */

enum { P_NONE = 0, P_TCP, P_UDP, P_ICMP };

typedef struct {
    int                proto;
    int                fd;
    struct sockaddr_in peer;
    uint16_t           my_id;     /* наш идентификатор, чтобы отсеивать свои же пакеты */
    uint16_t           my_seq;
} link_t;

static int resolve(const char *host, int port, struct sockaddr_in *out)
{
    struct addrinfo hints, *res;
    char service[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;     /* тип не важен, нужен только адрес */
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0)
        return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* 1 - прочитали n байт, 0 - соединение закрыто, -1 - ошибка */
static int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 1;
}

static uint16_t icmp_checksum(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (; n > 1; n -= 2, p += 2)
        sum += (uint32_t)(p[0] << 8 | p[1]);
    if (n) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return htons((uint16_t)~sum);       /* хранится в сетевом порядке */
}

/* --- TCP: один пир слушает (listen/accept), другой подключается (connect) --- */
static int tcp_open(link_t *l, const char *host, int lport, int pport, int sigfd)
{
    l->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (l->fd == -1) { perror("socket"); return -1; }

    if (host) {                                         /* активная сторона */
        if (resolve(host, pport, &l->peer) == -1) { fprintf(stderr, "не могу разрешить %s\n", host); return -1; }
        printf("подключаюсь к %s:%d (TCP)...\n", host, pport);
        if (connect(l->fd, (struct sockaddr *)&l->peer, sizeof l->peer) == -1) {
            if (errno != EINTR) { perror("connect"); return -1; }
            /* Сигнал прервал connect(), но соединение продолжает устанавливаться
             * в фоне: ждём, пока сокет станет доступен для записи, и не
             * забываем про сигналы (Ctrl+C отменяет подключение). */
            struct pollfd w[2] = { { l->fd, POLLOUT, 0 }, { sigfd, POLLIN, 0 } };
            for (;;) {
                if (poll(w, 2, -1) == -1) { if (errno == EINTR) continue; perror("poll"); return -1; }
                if ((w[1].revents & POLLIN) && sig_report(sigfd, "чат")) return -1;
                if (w[0].revents) break;
            }
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(l->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) { fprintf(stderr, "connect: %s\n", strerror(err)); return -1; }
        }
    } else {                                            /* пассивная сторона */
        int one = 1;
        setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

        struct sockaddr_in me = { .sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_ANY),
                                  .sin_port = htons((uint16_t)lport) };
        if (bind(l->fd, (struct sockaddr *)&me, sizeof me) == -1) { perror("bind"); return -1; }
        if (listen(l->fd, 1) == -1) { perror("listen"); return -1; }

        printf("жду подключения на TCP-порту %d ...\n", lport);
        struct pollfd p[2] = { { l->fd, POLLIN, 0 }, { sigfd, POLLIN, 0 } };
        for (;;) {
            if (poll(p, 2, -1) == -1) { if (errno == EINTR) continue; perror("poll"); return -1; }
            if ((p[1].revents & POLLIN) && sig_report(sigfd, "чат"))
                return -1;                              /* Ctrl+C во время ожидания */
            if (p[0].revents & POLLIN) {
                socklen_t sl = sizeof l->peer;
                int c = accept(l->fd, (struct sockaddr *)&l->peer, &sl);
                if (c == -1) { if (errno == EINTR) continue; perror("accept"); return -1; }
                close(l->fd);                           /* слушающий сокет больше не нужен */
                l->fd = c;
                break;
            }
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &l->peer.sin_addr, ip, sizeof ip);
        printf("подключился %s:%d\n", ip, ntohs(l->peer.sin_port));
    }
    int one = 1;
    setsockopt(l->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* не копить мелкие сообщения */
    return 0;
}

/* --- UDP: bind на свой порт, sendto на порт пира --- */
static int udp_open(link_t *l, const char *host, int lport, int pport)
{
    l->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (l->fd == -1) { perror("socket"); return -1; }
    if (resolve(host, pport, &l->peer) == -1) { fprintf(stderr, "не могу разрешить %s\n", host); return -1; }

    struct sockaddr_in me = { .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_ANY),
                              .sin_port = htons((uint16_t)lport) };
    if (bind(l->fd, (struct sockaddr *)&me, sizeof me) == -1) { perror("bind"); return -1; }
    printf("UDP: слушаю порт %d, пир %s:%d\n", lport, host, pport);
    return 0;
}

/* --- ICMP: сырой сокет, сообщения едут в поле данных Echo Request --- */
static int icmp_open(link_t *l, const char *host)
{
    l->fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);    /* нужен root / CAP_NET_RAW */
    if (l->fd == -1) {
        perror("socket(SOCK_RAW, IPPROTO_ICMP)");
        fprintf(stderr, "для ICMP запустите через sudo (или: sudo setcap cap_net_raw+ep ./chat)\n");
        return -1;
    }
    if (resolve(host, 0, &l->peer) == -1) { fprintf(stderr, "не могу разрешить %s\n", host); return -1; }

    uint16_t id;
    RAND_bytes((unsigned char *)&id, sizeof id);
    l->my_id = id;
    l->my_seq = 0;
    printf("ICMP: пир %s (id=0x%04x)\n", host, l->my_id);
    return 0;
}

static int link_send(link_t *l, const uint8_t *frame, int flen)
{
    switch (l->proto) {
    case P_TCP: {
        uint16_t n = htons((uint16_t)flen);
        if (send_all(l->fd, &n, sizeof n) == -1) return -1;
        return send_all(l->fd, frame, (size_t)flen);
    }
    case P_UDP:
        return sendto(l->fd, frame, (size_t)flen, 0,
                      (struct sockaddr *)&l->peer, sizeof l->peer) == -1 ? -1 : 0;
    case P_ICMP: {
        struct { struct icmphdr h; uint8_t data[MAX_FRAME]; } pkt;
        memset(&pkt, 0, sizeof pkt.h);
        pkt.h.type = ICMP_ECHO;                         /* Echo Request, как у ping */
        pkt.h.code = 0;
        pkt.h.un.echo.id = htons(l->my_id);
        pkt.h.un.echo.sequence = htons(++l->my_seq);
        memcpy(pkt.data, frame, (size_t)flen);
        pkt.h.checksum = 0;
        pkt.h.checksum = icmp_checksum(&pkt, sizeof pkt.h + (size_t)flen);
        /* заголовок IP ядро добавит само */
        return sendto(l->fd, &pkt, sizeof pkt.h + (size_t)flen, 0,
                      (struct sockaddr *)&l->peer, sizeof l->peer) == -1 ? -1 : 0;
    }
    }
    return -1;
}

/* >0 - длина кадра в out, 0 - пакет не наш (игнорируем), -1 - связь закрыта/ошибка */
static int link_recv(link_t *l, uint8_t *out, size_t cap)
{
    switch (l->proto) {
    case P_TCP: {
        uint16_t n;
        int r = recv_all(l->fd, &n, sizeof n);
        if (r <= 0) return -1;
        n = ntohs(n);
        if (n == 0 || n > cap) return -1;
        r = recv_all(l->fd, out, n);
        return r == 1 ? (int)n : -1;
    }
    case P_UDP: {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(l->fd, out, cap, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return errno == EINTR ? 0 : -1;
        if (from.sin_addr.s_addr != l->peer.sin_addr.s_addr ||
            from.sin_port != l->peer.sin_port)
            return 0;                                   /* пакет не от нашего пира */
        return (int)n;
    }
    case P_ICMP: {
        uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(l->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return errno == EINTR ? 0 : -1;

        size_t iphl = (size_t)(buf[0] & 0x0f) * 4;      /* raw-сокет отдаёт пакет вместе с IP-заголовком */
        if ((size_t)n < iphl + sizeof(struct icmphdr)) return 0;

        struct icmphdr h;
        memcpy(&h, buf + iphl, sizeof h);
        if (from.sin_addr.s_addr != l->peer.sin_addr.s_addr) return 0;
        if (h.type != ICMP_ECHO) return 0;              /* эхо-ответы ядра (type 0) и прочее - мимо */
        if (ntohs(h.un.echo.id) == l->my_id) return 0;/* это наш же исходящий пакет (loopback) */

        size_t plen = (size_t)n - iphl - sizeof h;
        if (plen > cap) return 0;
        memcpy(out, buf + iphl + sizeof h, plen);
        return (int)plen;
    }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

static void usage(const char *p)
{
    fprintf(stderr,
        "Использование:\n"
        "  %s -m tcp  -L <порт>                      ждать подключения\n"
        "  %s -m tcp  -H <адрес> -P <порт>           подключиться\n"
        "  %s -m udp  -L <свой порт> -H <адрес> -P <порт пира>\n"
        "  %s -m icmp -H <адрес>                     (нужен root)\n"
        "Опции: -k <пароль>  -n (без шифрования)  -h\n"
        "Без -m протокол выбирается интерактивно.\n", p, p, p, p);
}

static void ask(const char *q, char *buf, size_t n)
{
    printf("%s", q);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) buf[0] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
}

static void print_safe(const uint8_t *s, int n)     /* убираем управляющие символы */
{
    for (int i = 0; i < n; i++)
        putchar((s[i] >= 32 || s[i] == '\t' || s[i] >= 0x80) ? s[i] : '?');
}

int main(int argc, char **argv)
{
    int   proto = P_NONE, lport = 0, pport = 0, opt;
    char *host = NULL, *pass = NULL;

    while ((opt = getopt(argc, argv, "m:L:H:P:k:nh")) != -1) {
        switch (opt) {
        case 'm':
            proto = !strcmp(optarg, "tcp") ? P_TCP : !strcmp(optarg, "udp") ? P_UDP :
                    !strcmp(optarg, "icmp") ? P_ICMP : P_NONE;
            if (proto == P_NONE) { usage(argv[0]); return 1; }
            break;
        case 'L': lport = atoi(optarg); break;
        case 'H': host = optarg; break;
        case 'P': pport = atoi(optarg); break;
        case 'k': pass = optarg; break;
        case 'n': g_crypto = 0; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    /* --- выбор протокола --- */
    char tmp[128];
    if (proto == P_NONE) {
        puts("Выберите протокол:\n  1) TCP\n  2) UDP\n  3) ICMP (нужен root)");
        ask("> ", tmp, sizeof tmp);
        proto = atoi(tmp) == 1 ? P_TCP : atoi(tmp) == 2 ? P_UDP : atoi(tmp) == 3 ? P_ICMP : P_NONE;
        if (proto == P_NONE) { fputs("неверный выбор\n", stderr); return 1; }
    }

    /* --- недостающие параметры спрашиваем --- */
    static char hostbuf[128];
    if (proto == P_TCP && !host && !lport) {
        ask("Адрес пира (пусто = ждать подключения): ", hostbuf, sizeof hostbuf);
        if (hostbuf[0]) host = hostbuf;
    }
    if (proto != P_TCP && !host) {
        ask("Адрес пира: ", hostbuf, sizeof hostbuf);
        host = hostbuf;
    }
    if (proto == P_UDP && !lport) { ask("Свой порт: ", tmp, sizeof tmp); lport = atoi(tmp); }
    if (proto == P_TCP && !host && !lport) { ask("Порт для прослушивания: ", tmp, sizeof tmp); lport = atoi(tmp); }
    if (proto != P_ICMP && host && !pport) { ask("Порт пира: ", tmp, sizeof tmp); pport = atoi(tmp); }

    if ((proto == P_UDP && (!lport || !pport || !host)) ||
        (proto == P_TCP && !(lport || (host && pport))) ||
        (proto == P_ICMP && !host)) {
        usage(argv[0]);
        return 1;
    }

    /* --- ключ --- */
    if (g_crypto) {
        if (!pass) pass = getpass("Общий пароль (одинаковый у обоих): ");
        if (!pass || !*pass || derive_key(pass) == -1) { fputs("ошибка ключа\n", stderr); return 1; }
    } else {
        puts("!!! ШИФРОВАНИЕ ВЫКЛЮЧЕНО (-n): сообщения идут открытым текстом !!!");
    }

    int sigfd = sig_init();
    if (sigfd == -1) { perror("sig_init"); return 1; }

    /* --- соединение --- */
    link_t l;
    memset(&l, 0, sizeof l);
    l.proto = proto;
    l.fd = -1;
    int rc = proto == P_TCP ? tcp_open(&l, host, lport, pport, sigfd)
           : proto == P_UDP ? udp_open(&l, host, lport, pport)
                            : icmp_open(&l, host);
    if (rc == -1) { if (l.fd != -1) close(l.fd); return 1; }

    printf("Чат запущен (%s, %s). Пишите сообщения; /quit или Ctrl+C - выход.\n",
           proto == P_TCP ? "TCP" : proto == P_UDP ? "UDP" : "ICMP",
           g_crypto ? "AES-256-GCM" : "без шифрования");

    /* --- главный цикл: stdin + сокет + сигналы --- */
    struct pollfd p[3] = { { STDIN_FILENO, POLLIN, 0 }, { l.fd, POLLIN, 0 }, { sigfd, POLLIN, 0 } };
    uint8_t frame[MAX_FRAME + 16], plain[MAX_FRAME];
    int running = 1;

    while (running) {
        if (poll(p, 3, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (p[2].revents & POLLIN) {
            if (sig_report(sigfd, "чат")) { puts("Выход по сигналу."); break; }
        }

        if (p[0].revents & (POLLIN | POLLHUP)) {
            char in[MAX_MSG];
            ssize_t n = read(STDIN_FILENO, in, sizeof in);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;                           /* Ctrl+D */
            /* в буфере может быть несколько строк - шлём каждую отдельно */
            char *line = in, *end = in + n;
            while (line < end) {
                char *nl = memchr(line, '\n', (size_t)(end - line));
                size_t len = nl ? (size_t)(nl - line) : (size_t)(end - line);
                if (len == 5 && !memcmp(line, "/quit", 5)) { running = 0; break; }
                if (len > 0) {
                    int fl = seal((uint8_t *)line, (int)len, frame);
                    if (fl < 0 || link_send(&l, frame, fl) == -1)
                        perror("отправка");
                }
                line += len + (nl ? 1 : 0);
            }
        }

        if (running && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            int fl = link_recv(&l, frame, sizeof frame);
            if (fl < 0) { puts("Связь закрыта пиром."); break; }
            if (fl > 0) {
                int n = unseal(frame, fl, plain);
                if (n < 0) {
                    puts("[!] не удалось расшифровать/проверить сообщение (чужой пакет, другой ключ или подмена)");
                } else {
                    fputs("пир> ", stdout);
                    print_safe(plain, n);
                    putchar('\n');
                    fflush(stdout);
                }
            }
        }
    }

    close(l.fd);
    return 0;
}
