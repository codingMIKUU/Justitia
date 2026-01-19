#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_LEN
#define MSG_LEN 24
#endif

#define HOSTNAME_PATH "/proc/sys/kernel/hostname"

static char *get_sock_path(void)
{
    FILE *fp = fopen(HOSTNAME_PATH, "r");
    if (!fp) {
        return NULL;
    }

    char hostname[100];
    if (!fgets(hostname, sizeof(hostname), fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    int len = (int)strlen(hostname);
    if (len > 0 && hostname[len - 1] == '\n')
        hostname[len - 1] = '\0';

    strcat(hostname, "_rdma_socket");

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char *sock_path = (char *)calloc(108, sizeof(char));
    if (!sock_path)
        return NULL;

    snprintf(sock_path, 108, "%s/%s", home, hostname);
    return sock_path;
}

static int connect_uds(const char *sock_path)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == -1) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un remote;
    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    strncpy(remote.sun_path, sock_path, sizeof(remote.sun_path) - 1);

    size_t len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if (connect(s, (struct sockaddr *)&remote, (socklen_t)len) == -1) {
        perror("connect");
        close(s);
        return -1;
    }

    return s;
}

static int do_join_and_get_slot(const char *sock_path, pid_t pid, pid_t tid)
{
    int s = connect_uds(sock_path);
    if (s < 0)
        return -1;

    char buf[MSG_LEN];
    memset(buf, 0, sizeof(buf));

    // join:%016Lx
    unsigned long long vaddr = 0;
    snprintf(buf, sizeof(buf), "join:%016llx", vaddr);
    if (send(s, buf, strlen(buf), 0) == -1) {
        perror("send join");
        close(s);
        return -1;
    }

    // receive sender/recver prompt
    int n = (int)recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        perror("recv prompt");
        close(s);
        return -1;
    }
    buf[n] = '\0';
    if (strncmp(buf, "sender:", 7) != 0 && strcmp(buf, "recver") != 0) {
        fprintf(stderr, "Unexpected prompt: '%s'\n", buf);
        close(s);
        return -1;
    }

    // send pid:tid
    memset(buf, 0, sizeof(buf));
    int msg_len = snprintf(buf, sizeof(buf), "%d:%d", (int)pid, (int)tid);
    if (send(s, buf, (size_t)msg_len, 0) == -1) {
        perror("send pid:tid");
        close(s);
        return -1;
    }

    // receive slot
    n = (int)recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        perror("recv slot");
        close(s);
        return -1;
    }
    buf[n] = '\0';

    close(s);
    return (int)strtol(buf, NULL, 10);
}

static int do_leave(const char *sock_path, pid_t pid, pid_t tid)
{
    int s = connect_uds(sock_path);
    if (s < 0)
        return -1;

    char buf[MSG_LEN];
    int len = snprintf(buf, sizeof(buf), "l:%d:%d", (int)pid, (int)tid);
    if (send(s, buf, (size_t)len, 0) == -1) {
        perror("send leave");
        close(s);
        return -1;
    }

    close(s);
    return 0;
}

struct thread_arg {
    const char *sock_path;
    int index;
    int do_leave;
    int slot_first;
    int slot_second;
    int rc;
};

static pthread_barrier_t join_barrier;

static void *thread_main(void *arg)
{
    struct thread_arg *a = (struct thread_arg *)arg;

    pid_t pid = getpid();
    pid_t tid = (pid_t)syscall(SYS_gettid);

    a->slot_first = do_join_and_get_slot(a->sock_path, pid, tid);
    if (a->slot_first < 0) {
        a->rc = 1;
        return NULL;
    }

    // Join again; should return same slot for same (pid,tid)
    a->slot_second = do_join_and_get_slot(a->sock_path, pid, tid);
    if (a->slot_second < 0) {
        a->rc = 2;
        return NULL;
    }

    /* Hold the slot until all threads have joined, so we can validate uniqueness
     * under concurrent (pid,tid) registrations.
     */
    pthread_barrier_wait(&join_barrier);

    if (a->do_leave) {
        if (do_leave(a->sock_path, pid, tid) != 0) {
            a->rc = 3;
            return NULL;
        }
    }

    a->rc = 0;
    return NULL;
}

static int cmp_int(const void *a, const void *b)
{
    const int ia = *(const int *)a;
    const int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

int main(int argc, char **argv)
{
    int num_threads = 16;
    if (argc >= 2)
        num_threads = atoi(argv[1]);
    if (num_threads <= 0 || num_threads > 256) {
        fprintf(stderr, "num_threads must be 1..256\n");
        return 2;
    }

    char *sock_path = get_sock_path();
    if (!sock_path) {
        fprintf(stderr, "Failed to derive sock path (HOME/hostname).\n");
        return 2;
    }

    printf("sock_path=%s\n", sock_path);

    pthread_t *ths = (pthread_t *)calloc((size_t)num_threads, sizeof(pthread_t));
    struct thread_arg *args = (struct thread_arg *)calloc((size_t)num_threads, sizeof(struct thread_arg));
    int *slots = (int *)calloc((size_t)num_threads, sizeof(int));

    if (!ths || !args || !slots) {
        fprintf(stderr, "alloc failed\n");
        return 2;
    }

    if (pthread_barrier_init(&join_barrier, NULL, (unsigned)num_threads) != 0) {
        perror("pthread_barrier_init");
        return 2;
    }

    // Round 1: join+join+leave
    for (int i = 0; i < num_threads; i++) {
        args[i].sock_path = sock_path;
        args[i].index = i;
        args[i].do_leave = 1;
        args[i].slot_first = -1;
        args[i].slot_second = -1;
        args[i].rc = -1;
        if (pthread_create(&ths[i], NULL, thread_main, &args[i]) != 0) {
            perror("pthread_create");
            return 2;
        }
    }
    for (int i = 0; i < num_threads; i++)
        pthread_join(ths[i], NULL);

    int failed = 0;
    for (int i = 0; i < num_threads; i++) {
        if (args[i].rc != 0) {
            fprintf(stderr, "thread %d failed rc=%d\n", i, args[i].rc);
            failed = 1;
        }
        if (args[i].slot_first != args[i].slot_second) {
            fprintf(stderr, "thread %d slot mismatch: %d vs %d\n", i, args[i].slot_first, args[i].slot_second);
            failed = 1;
        }
        slots[i] = args[i].slot_first;
    }

    qsort(slots, (size_t)num_threads, sizeof(int), cmp_int);
    int dup = 0;
    for (int i = 1; i < num_threads; i++) {
        if (slots[i] == slots[i - 1])
            dup = 1;
    }

    printf("round1 slots:");
    for (int i = 0; i < num_threads; i++)
        printf(" %d", slots[i]);
    printf("\n");

    if (dup) {
        fprintf(stderr, "round1: found duplicate slots across threads (unexpected for scheme A)\n");
        failed = 1;
    }

    // Round 2: join+join+leave again; slots should be reusable
    for (int i = 0; i < num_threads; i++) {
        args[i].do_leave = 1;
        args[i].slot_first = -1;
        args[i].slot_second = -1;
        args[i].rc = -1;
        if (pthread_create(&ths[i], NULL, thread_main, &args[i]) != 0) {
            perror("pthread_create");
            return 2;
        }
    }
    for (int i = 0; i < num_threads; i++)
        pthread_join(ths[i], NULL);

    for (int i = 0; i < num_threads; i++)
        slots[i] = args[i].slot_first;
    qsort(slots, (size_t)num_threads, sizeof(int), cmp_int);

    printf("round2 slots:");
    for (int i = 0; i < num_threads; i++)
        printf(" %d", slots[i]);
    printf("\n");

    pthread_barrier_destroy(&join_barrier);
    free(sock_path);
    free(ths);
    free(args);
    free(slots);

    return failed ? 1 : 0;
}
