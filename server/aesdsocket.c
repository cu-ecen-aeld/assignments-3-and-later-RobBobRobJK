#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"
#define BUFSIZE 1024

static int server_fd = -1;
static volatile int caught_signal = 0;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static timer_t timer_id;

/* Singly linked list node for tracking threads */
typedef struct thread_node {
    pthread_t thread;
    int client_fd;
    int complete;
    struct thread_node *next;
} thread_node_t;

static thread_node_t *thread_list = NULL;

void signal_handler(int sig)
{
    syslog(LOG_INFO, "Caught signal, exiting");
    caught_signal = 1;
    if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
}

/* Timer callback - appends RFC 2822 timestamp to data file every 10 seconds */
void timer_handler(union sigval sv)
{
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (!tmp) return;

    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), "timestamp:%a, %d %b %Y %T %z\n", tmp);

    pthread_mutex_lock(&file_mutex);
    FILE *f = fopen(DATAFILE, "a");
    if (f) {
        fputs(timebuf, f);
        fclose(f);
    }
    pthread_mutex_unlock(&file_mutex);
}

void init_timer(void)
{
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_notify_attributes = NULL;

    if (timer_create(CLOCK_REALTIME, &sev, &timer_id) != 0) {
        syslog(LOG_ERR, "timer_create failed: %s", strerror(errno));
        return;
    }

    struct itimerspec its;
    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timer_id, 0, &its, NULL) != 0) {
        syslog(LOG_ERR, "timer_settime failed: %s", strerror(errno));
    }
}

/* Thread function - handles one client connection */
void *thread_func(void *arg)
{
    thread_node_t *node = (thread_node_t *)arg;
    int cfd = node->client_fd;
    char *buf = NULL;
    size_t buf_used = 0;
    char tmp[BUFSIZE];
    ssize_t bytes;

    /* Receive until newline */
    while (1) {
        bytes = recv(cfd, tmp, sizeof(tmp), 0);
        if (bytes <= 0) goto cleanup;

        char *newbuf = realloc(buf, buf_used + bytes + 1);
        if (!newbuf) { syslog(LOG_ERR, "realloc failed"); goto cleanup; }
        buf = newbuf;
        memcpy(buf + buf_used, tmp, bytes);
        buf_used += bytes;
        buf[buf_used] = '\0';

        if (memchr(tmp, '\n', bytes)) break;
    }

    /* Append to file under mutex */
    pthread_mutex_lock(&file_mutex);
    FILE *f = fopen(DATAFILE, "a");
    if (f) {
        fwrite(buf, 1, buf_used, f);
        fclose(f);
    }

    /* Send full file back under same mutex */
    f = fopen(DATAFILE, "r");
    if (f) {
        while ((bytes = fread(tmp, 1, sizeof(tmp), f)) > 0) {
            send(cfd, tmp, bytes, 0);
        }
        fclose(f);
    }
    pthread_mutex_unlock(&file_mutex);

cleanup:
    free(buf);
    close(cfd);
    node->complete = 1;
    return node;
}

void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) { perror("setsid"); exit(EXIT_FAILURE); }
    pid = fork();
    if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
    if (pid > 0) exit(EXIT_SUCCESS);
    umask(0);
    chdir("/");
    int fd = open("/dev/null", O_RDWR);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}

/* Join and free all completed threads from the list */
void cleanup_threads(void)
{
    thread_node_t *cur = thread_list;
    while (cur) {
        thread_node_t *next = cur->next;
        pthread_join(cur->thread, NULL);
        free(cur);
        cur = next;
    }
    thread_list = NULL;
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) daemon_mode = 1;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    if (listen(server_fd, 10) < 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    if (daemon_mode) daemonize();

    init_timer();

    while (!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (caught_signal) break;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        /* Allocate thread node and spawn thread */
        thread_node_t *node = malloc(sizeof(thread_node_t));
        if (!node) {
            syslog(LOG_ERR, "malloc failed");
            close(client_fd);
            continue;
        }
        node->client_fd = client_fd;
        node->complete = 0;
        node->next = thread_list;
        thread_list = node;

        if (pthread_create(&node->thread, NULL, thread_func, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            close(client_fd);
            free(node);
            thread_list = thread_list->next;
            continue;
        }

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    /* Cleanup on signal */
    timer_delete(timer_id);
    cleanup_threads();
    close(server_fd);
    server_fd = -1;
    pthread_mutex_destroy(&file_mutex);
    remove(DATAFILE);
    closelog();
    return 0;
}
