#ifndef PACER_H
#define PACER_H

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <semaphore.h>
#include <inttypes.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include "mlx4.h"

#define SHARED_MEM_NAME "/rdma-fairness"
#define SOCK_PATH "/users/yiwenzhg/rdma_socket"
#define MSG_LEN 24
#define MAX_FLOWS 512
#define HOSTNAME_PATH "/proc/sys/kernel/hostname"

struct flow_info {
    uint8_t pending;
    uint8_t active;
    uint8_t read;
};

struct shared_block {
    struct flow_info flows[MAX_FLOWS];
    uint32_t active_chunk_size;
    uint32_t active_chunk_size_read;
    uint32_t active_batch_ops;
    uint32_t virtual_link_cap;
    //uint16_t num_active_split_qps;         /* added to dynamically change number of split qps */
    uint16_t num_active_big_flows;         /* incremented when an elephant first sends a message */
    uint16_t num_active_small_flows;       /* incremented when a mouse first sends a message */
    uint16_t num_active_bw_flows;         /* incremented when an elephant first sends a message */
    uint16_t split_level;
};

extern __thread struct flow_info *flow;     /* per-thread flow slot; initialization in verbs.c */
extern struct shared_block *sb;            /* process-wide shared memory mapping; initialization in verbs.c */
extern __thread int start_flag;            /* per-thread */
extern __thread unsigned int slot;         /* per-thread slot id */
extern int start_recv;             /* initialized in qp.c */
extern __thread int isSmall;                /* per-thread class */
extern __thread int num_active_small_flows; /* per-thread counters for cleanup */
extern __thread int num_active_big_flows;   /* per-thread counters for cleanup */
extern __thread int justitia_exit_done;     /* per-thread: ensure exit handler runs once */
#ifdef CPU_FRIENDLY
extern __thread unsigned int flow_socket;
extern double cpu_mhz;              /* declaration; initialization in verbs.c */
#endif

char *get_sock_path();
//void contact_pacer(int join, uint64_t vaddr);
void contact_pacer(int join);
void set_inactive_on_exit();
void termination_handler(int sig);

#endif  /* pacer.h */
