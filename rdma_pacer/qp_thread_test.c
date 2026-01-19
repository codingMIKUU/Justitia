#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <infiniband/verbs.h>

struct thread_arg {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    int qp_per_thread;
    long qp_class; /* 0=bw, 1=lat, 2=tput */
    int rc;
};

static void *thread_main(void *arg)
{
    struct thread_arg *a = (struct thread_arg *)arg;

    for (int i = 0; i < a->qp_per_thread; i++) {
        struct ibv_qp_init_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.send_cq = a->cq;
        attr.recv_cq = a->cq;
        attr.qp_type = IBV_QPT_RC;
        attr.cap.max_send_wr = 1;
        attr.cap.max_recv_wr = 1;
        attr.cap.max_send_sge = 1;
        attr.cap.max_recv_sge = 1;
        attr.qp_context = (void *)a->qp_class;

        struct ibv_qp *qp = ibv_create_qp(a->pd, &attr);
        if (!qp) {
            fprintf(stderr, "ibv_create_qp failed: %s\n", strerror(errno));
            a->rc = 1;
            return NULL;
        }

        if (ibv_destroy_qp(qp) != 0) {
            fprintf(stderr, "ibv_destroy_qp failed: %s\n", strerror(errno));
            a->rc = 2;
            return NULL;
        }
    }

    a->rc = 0;
    return NULL;
}

static struct ibv_device *find_device(const char *name)
{
    int num = 0;
    struct ibv_device **list = ibv_get_device_list(&num);
    if (!list) {
        fprintf(stderr, "ibv_get_device_list failed\n");
        return NULL;
    }

    struct ibv_device *found = NULL;
    for (int i = 0; i < num; i++) {
        const char *dev_name = ibv_get_device_name(list[i]);
        if (!name || strcmp(name, dev_name) == 0) {
            found = list[i];
            break;
        }
    }

    /* Note: found points into list; we keep list until open_device */
    if (!found) {
        fprintf(stderr, "No matching device found. Available devices:\n");
        for (int i = 0; i < num; i++)
            fprintf(stderr, "  %s\n", ibv_get_device_name(list[i]));
    }

    /* Open needs list alive only until open returns; then safe to free */
    struct ibv_device *ret = found;
    ibv_free_device_list(list);
    return ret;
}

int main(int argc, char **argv)
{
    const char *dev_name = NULL;
    int num_threads = 16;
    int qp_per_thread = 2;
    long qp_class = 0;

    if (argc >= 2)
        dev_name = argv[1];
    if (argc >= 3)
        num_threads = atoi(argv[2]);
    if (argc >= 4)
        qp_per_thread = atoi(argv[3]);
    if (argc >= 5)
        qp_class = strtol(argv[4], NULL, 10);

    if (num_threads <= 0 || num_threads > 256) {
        fprintf(stderr, "num_threads must be 1..256\n");
        return 2;
    }
    if (qp_per_thread <= 0 || qp_per_thread > 1024) {
        fprintf(stderr, "qp_per_thread must be 1..1024\n");
        return 2;
    }

    struct ibv_device *dev = find_device(dev_name);
    if (!dev)
        return 2;

    struct ibv_context *ctx = ibv_open_device(dev);
    if (!ctx) {
        fprintf(stderr, "ibv_open_device failed: %s\n", strerror(errno));
        return 2;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "ibv_alloc_pd failed: %s\n", strerror(errno));
        ibv_close_device(ctx);
        return 2;
    }

    struct ibv_cq *cq = ibv_create_cq(ctx, 64, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "ibv_create_cq failed: %s\n", strerror(errno));
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 2;
    }

    printf("Using device=%s threads=%d qp_per_thread=%d qp_class=%ld\n",
           ibv_get_device_name(dev), num_threads, qp_per_thread, qp_class);

    pthread_t *ths = calloc((size_t)num_threads, sizeof(*ths));
    struct thread_arg *args = calloc((size_t)num_threads, sizeof(*args));
    if (!ths || !args) {
        fprintf(stderr, "alloc failed\n");
        return 2;
    }

    for (int i = 0; i < num_threads; i++) {
        args[i].pd = pd;
        args[i].cq = cq;
        args[i].qp_per_thread = qp_per_thread;
        args[i].qp_class = qp_class;
        args[i].rc = -1;
        if (pthread_create(&ths[i], NULL, thread_main, &args[i]) != 0) {
            perror("pthread_create");
            return 2;
        }
    }

    int failed = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(ths[i], NULL);
        if (args[i].rc != 0)
            failed = 1;
    }

    free(args);
    free(ths);

    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);

    return failed ? 1 : 0;
}
