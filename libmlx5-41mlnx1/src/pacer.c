
#include "pacer.h"

#include <sys/syscall.h>


char *get_sock_path() {
    FILE *fp;
    fp = fopen(HOSTNAME_PATH, "r");
    if (fp == NULL) {
        printf("Error opening %s, use default SOCK_PATH", HOSTNAME_PATH);
        fclose(fp);
        return SOCK_PATH;
    }

    char hostname[100];
    if(fgets(hostname, 100, fp) != NULL) {
        char *sock_path = (char *)calloc(108, sizeof(char));
        int len = strlen(hostname);
        if (len > 0 && hostname[len-1] == '\n') hostname[len-1] = '\0';
        strcat(hostname, "_rdma_socket");
        strcpy(sock_path, getenv("HOME"));
        len = strlen(sock_path);
        sock_path[len] = '/';
        strcat(sock_path, hostname);
        fclose(fp);
        return sock_path;
    }

    fclose(fp);
    return SOCK_PATH;
}

// join=0 -> exit_app_*; join=1 -> join + get slot; join=2 -> app_*; join=3 -> deregister slot mapping
void contact_pacer(int join) {
    char *sock_path = get_sock_path();
    unsigned int s, len;
    struct sockaddr_un remote;
    char str[MSG_LEN];
    long long unsigned int vaddr = 0;     // hack for now
    int vaddr_idx;

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, sock_path);
    free(sock_path);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if (connect(s, (struct sockaddr *)&remote, len) == -1) {
        perror("connect");
        exit(1);
    }

    if (join == 0) {
        memset(str, 0, MSG_LEN);
        if (isSmall == 0) {
            strcpy(str, "exit_app_bw");
        } else if (isSmall == 1) {
            strcpy(str, "exit_app_lat");
        } else if (isSmall == 2) {
            strcpy(str, "exit_app_tput");
        } else {
            strcpy(str, "exit_app_bw");
        }
        if (send(s, str, strlen(str), 0) == -1) {
            perror("send: exit");
            exit(1);
        }
        close(s);
        return;
    }

    if (join == 1) {
        sprintf(str, "join:%016Lx", vaddr);
        if (send(s, str, strlen(str), 0) == -1) {
            perror("send: join");
            exit(1);
        }

        if ((len = recv(s, str, MSG_LEN, 0)) > 0) {
            str[len] = '\0';
            if (strncmp(str, "sender:xxx", 6) == 0) {
                sscanf(str, "sender:%x", &vaddr_idx);
                (void)vaddr_idx;
            } else if (strcmp(str, "recver") != 0) {
                printf("unrecognized string. must be \"sender\" or \"recver\"\n");
                exit(1);
            }
        } else {
            if (len < 0) perror("recv");
            else printf("Server closed connection\n");
            exit(1);
        }

        pid_t my_pid = getpid();
        pid_t my_tid = (pid_t)syscall(SYS_gettid);
        len = snprintf(str, MSG_LEN, "%d:%d", my_pid, my_tid);
        if (send(s, str, len, 0) == -1) {
            perror("send: pid:tid");
            exit(1);
        }

        if ((len = recv(s, str, MSG_LEN, 0)) > 0) {
            str[len] = '\0';
        } else {
            if (len < 0) perror("recv");
            else printf("Server closed connection\n");
            exit(1);
        }
        slot = strtol(str, NULL, 10);

#ifdef CPU_FRIENDLY
        flow_socket = s;
        return;
#else
        close(s);
        return;
#endif
    }

    if (join == 2) {
        memset(str, 0, MSG_LEN);
        if (isSmall == 0) {
            strcpy(str, "app_bw");
        } else if (isSmall == 1) {
            strcpy(str, "app_lat");
        } else if (isSmall == 2) {
            strcpy(str, "app_tput");
        } else {
            printf("unrecognized app type. Exit\n");
            exit(1);
        }
        if (send(s, str, strlen(str), 0) == -1) {
            perror("send: app type");
            exit(1);
        }
        close(s);
        return;
    }

    if (join == 3) {
        pid_t my_pid = getpid();
        pid_t my_tid = (pid_t)syscall(SYS_gettid);
        len = snprintf(str, MSG_LEN, "l:%d:%d", my_pid, my_tid);
        if (send(s, str, len, 0) == -1) {
            perror("send: leave");
            exit(1);
        }
        close(s);
        return;
    }

    close(s);
}

void set_inactive_on_exit() {
    /* make exit handler idempotent per-thread */
    if (justitia_exit_done)
        return;

    if (!flow)
        return;

    if (isSmall == 1) {
        if (num_active_small_flows)
            __atomic_fetch_sub(&sb->num_active_small_flows, num_active_small_flows, __ATOMIC_RELAXED);
        contact_pacer(0);
    } else if (__atomic_load_n(&flow->read, __ATOMIC_RELAXED)) {
        __atomic_store_n(&flow->read, 0, __ATOMIC_RELAXED);
        contact_pacer(0);
    } else {
        if (num_active_big_flows)
            __atomic_fetch_sub(&sb->num_active_big_flows, num_active_big_flows, __ATOMIC_RELAXED);
        contact_pacer(0);
    }

    __atomic_store_n(&flow->pending, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&flow->active, 0, __ATOMIC_RELAXED);

#ifdef CPU_FRIENDLY
    if (flow_socket) {
        close(flow_socket);
        flow_socket = 0;
    }
#endif

    contact_pacer(3);

    flow = NULL;
    slot = 0;
    start_flag = 1;
    num_active_small_flows = 0;
    num_active_big_flows = 0;

    justitia_exit_done = 1;
}

void termination_handler(int sig) {
    set_inactive_on_exit();
    _exit(1);
}
