#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define FIFO_PATH "/tmp/engine_fifo"
#define STACK_SIZE (1024 * 1024)
#define BUFFER_SIZE 10
#define CHUNK_SIZE 256

// ================= BUFFER =================
typedef struct {
    char data[BUFFER_SIZE][CHUNK_SIZE];
    int in, out, count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} buffer_t;

buffer_t buffer;

void buffer_init() {
    buffer.in = buffer.out = buffer.count = 0;
    pthread_mutex_init(&buffer.lock, NULL);
    pthread_cond_init(&buffer.not_empty, NULL);
    pthread_cond_init(&buffer.not_full, NULL);
}

void buffer_put(char *data) {
    pthread_mutex_lock(&buffer.lock);

    while (buffer.count == BUFFER_SIZE)
        pthread_cond_wait(&buffer.not_full, &buffer.lock);

    strcpy(buffer.data[buffer.in], data);
    buffer.in = (buffer.in + 1) % BUFFER_SIZE;
    buffer.count++;

    pthread_cond_signal(&buffer.not_empty);
    pthread_mutex_unlock(&buffer.lock);
}

void buffer_get(char *dest) {
    pthread_mutex_lock(&buffer.lock);

    while (buffer.count == 0)
        pthread_cond_wait(&buffer.not_empty, &buffer.lock);

    strcpy(dest, buffer.data[buffer.out]);
    buffer.out = (buffer.out + 1) % BUFFER_SIZE;
    buffer.count--;

    pthread_cond_signal(&buffer.not_full);
    pthread_mutex_unlock(&buffer.lock);
}

// ================= THREADS =================
void *producer(void *arg) {
    int fd = *(int *)arg;
    char temp[CHUNK_SIZE];

    while (1) {
        int n = read(fd, temp, CHUNK_SIZE - 1);
        if (n <= 0) break;

        temp[n] = '\0';
        buffer_put(temp);
    }
    return NULL;
}

void *consumer(void *arg) {
    int log_fd = open("/tmp/container.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
    char temp[CHUNK_SIZE];

    while (1) {
        buffer_get(temp);
        write(log_fd, temp, strlen(temp));
    }
    return NULL;
}

// ================= CONTAINER =================
typedef struct {
    char id[32];
    pid_t pid;
} container_t;

container_t containers[100];
int container_count = 0;

void add_container(const char *id, pid_t pid) {
    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    container_count++;
}

void list_containers() {
    printf("Running Containers:\n");
    for (int i = 0; i < container_count; i++) {
        printf("ID: %s | PID: %d\n", containers[i].id, containers[i].pid);
    }
}

void stop_container(const char *id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0) {
            kill(containers[i].pid, SIGKILL);
            printf("Stopped container %s\n", id);

            containers[i] = containers[container_count - 1];
            container_count--;
            return;
        }
    }
}

// ================= KERNEL =================
void register_with_kernel(pid_t pid, const char *id) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("monitor open");
        return;
    }

    struct monitor_request req;

    req.pid = pid;
    req.soft_limit_bytes = 20 * 1024 * 1024;
    req.hard_limit_bytes = 40 * 1024 * 1024;
    strncpy(req.container_id, id, MONITOR_NAME_LEN);

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl");
    }

    close(fd);
}

// ================= CHILD =================
int child_func(void *arg) {
    char **argv = (char **)arg;
    int *pipefd = (int *)argv[2];

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);

    close(pipefd[0]);
    close(pipefd[1]);

    if (chroot(argv[0]) != 0) {
        perror("chroot failed");
        return 1;
    }

    chdir("/");

    char *exec_args[] = {argv[1], NULL};
    execv(argv[1], exec_args);

    perror("exec failed");
    return 1;
}

// ================= SUPERVISOR =================
void run_supervisor() {
    unlink(FIFO_PATH);
    mkfifo(FIFO_PATH, 0666);

    buffer_init();

    printf("Supervisor running...\n");

    while (1) {
        char cmd_buf[256] = {0};

        int fd = open(FIFO_PATH, O_RDONLY);
        int n = read(fd, cmd_buf, sizeof(cmd_buf) - 1);
        close(fd);

        if (n <= 0) continue;

        cmd_buf[n] = '\0';

        printf(">>> Received: %s\n", cmd_buf);

        char *cmd = strtok(cmd_buf, " \n");

        if (!cmd) continue;

        if (strcmp(cmd, "start") == 0) {
            char *id = strtok(NULL, " ");
            char *rootfs = strtok(NULL, " ");
            char *exec = strtok(NULL, " ");

            int pipefd[2];
            pipe(pipefd);

            pthread_t prod, cons;
            pthread_create(&prod, NULL, producer, &pipefd[0]);
            pthread_create(&cons, NULL, consumer, NULL);

            char *stack = malloc(STACK_SIZE);
            char *stackTop = stack + STACK_SIZE;

            char *args[] = {rootfs, exec, (char *)pipefd};

            pid_t pid = clone(child_func, stackTop,
                CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                args);

            if (pid > 0) {
                close(pipefd[1]);

                printf("Started container %s with PID %d\n", id, pid);

                add_container(id, pid);
                register_with_kernel(pid, id);   // 🔥 kernel integration
            }
        }

        else if (strcmp(cmd, "ps") == 0) {
            list_containers();
        }

        else if (strcmp(cmd, "stop") == 0) {
            char *id = strtok(NULL, " ");
            if (id) stop_container(id);
        }
    }
}

// ================= CLIENT =================
void send_command(int argc, char *argv[]) {
    int fd = open(FIFO_PATH, O_WRONLY);

    char buffer[256] = {0};

    for (int i = 1; i < argc; i++) {
        strcat(buffer, argv[i]);
        strcat(buffer, " ");
    }

    write(fd, buffer, strlen(buffer));
    close(fd);
}

// ================= MAIN =================
int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    } else {
        send_command(argc, argv);
    }

    return 0;
}
