cat > main.c « 'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define N          3
#define OPEN_SEC   2
#define CLOSE_SEC  3

static volatile sig_atomic_t @id58593139 (*sent1), *sent2;
static volatile sig_atomic_t recv1 = 0, recv2 = 0, stop = 0;
static pid_t kid[N];
static sigset_t blk, old, empty;

static void h_usr1(int s)  { (void)s; recv1++; }
static void h_usr2(int s)  { (void)s; recv2++; }
static void h_int(int s)   { (void)s; stop = 1; }
static void h_alarm(int s) { (void)s; }
static void h_term(int s)  { (void)s; _exit(0); }

static void install(int signo, void (*h)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    if (sigaction(signo, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void setup_shared_memory(void)
{
    size_t sz = N * sizeof(sig_atomic_t);

    sent1 = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sent1 == MAP_FAILED) { perror("mmap sent1"); exit(EXIT_FAILURE); }

    sent2 = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sent2 == MAP_FAILED) { perror("mmap sent2"); exit(EXIT_FAILURE); }

    memset((void *)sent1, 0, sz);
    memset((void *)sent2, 0, sz);
}

static void cleanup_shared_memory(void)
{
    size_t sz = N * sizeof(sig_atomic_t);
    munmap((void *)sent1, sz);
    munmap((void *)sent2, sz);
}

static void setup_parent_handlers(void)
{
    install(SIGUSR1, h_usr1,  SA_RESTART);
    install(SIGUSR2, h_usr2,  SA_RESTART);
    install(SIGINT,  h_int,   SA_RESTART);
    install(SIGALRM, h_alarm, 0);
}

static void setup_child_handlers(void)
{
    install(SIGINT,  SIG_IGN, 0);
    install(SIGUSR1, SIG_IGN, 0);
    install(SIGUSR2, SIG_IGN, 0);
    install(SIGTERM, h_term,  0);
}

static void setup_signal_masks(void)
{
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigaddset(&blk, SIGUSR2);
    sigemptyset(&empty);
}

static void child_loop(int id)
{
    srand((unsigned)(getpid() ^ (id * 2654435761u)));
    for (;;) {
        usleep((useconds_t)((100 + rand() % 301) * 1000));
        if (rand() & 1) {
            sent1[id]++;
            kill(getppid(), SIGUSR1);
        } else {
            sent2[id]++;
            kill(getppid(), SIGUSR2);
        }
    }
}

static void child_main(int id)
{
    setup_child_handlers();
    child_loop(id);
}

static void spawn_children(void)
{
    for (int i = 0; i < N; i++) {
        kid[i] = fork();
        if (kid[i] < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (kid[i] == 0)
            child_main(i);
    }
}

static void sum_sent(int *s1, int *s2)
{
    *s1 = 0; *s2 = 0;
    for (int i = 0; i < N; i++) { *s1 += sent1[i]; *s2 += sent2[i]; }
}

static void print_snapshot(const char *label)
{
    int s1, s2;
    sum_sent(&s1, &s2);
    printf("%s recv=(%d,%d) sent=(%d,%d)\n",
           label, recv1, recv2, s1, s2);
}

static void phase_open(void)
{
    print_snapshot("[OPEN]   начало:");

    time_t t0 = time(NULL);
    while (!stop && time(NULL) - t0 < OPEN_SEC) {
        alarm(1);
        if (sigsuspend(&empty) == -1 && errno != EINTR)
            perror("sigsuspend");
    }
    alarm(0);

    print_snapshot("[OPEN]   конец: ");
}

static void phase_closed(void)
{
    if (sigprocmask(SIG_BLOCK, &blk, &old) == -1)
        perror("sigprocmask block");

    printf("[CLOSED] сигналы заблокированы на %d с...\n", CLOSE_SEC);

    struct timespec ts = { CLOSE_SEC, 0 };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;

    sigset_t pend;
    if (sigpending(&pend) == -1)
        perror("sigpending");

    printf("[CLOSED] pending: USR1=%d USR2=%d\n",
           sigismember(&pend, SIGUSR1) != 0,
