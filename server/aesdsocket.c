#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"
#define BUFSIZE 1024

static int server_fd = -1;
static int client_fd = -1;
static volatile int caught_signal = 0;

void signal_handler(int sig)
{
    syslog(LOG_INFO, "Caught signal, exiting");
    caught_signal = 1;
    if (client_fd != -1) shutdown(client_fd, SHUT_RDWR);
    if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
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

int handle_connection(int cfd, const char *client_ip)
{
    char *buf = NULL;
    size_t buf_size = 0;
    size_t buf_used = 0;
    char tmp[BUFSIZE];
    ssize_t bytes;
    int rc = 0;

    /* Receive until newline */
    while (1) {
        bytes = recv(cfd, tmp, sizeof(tmp), 0);
        if (bytes <= 0) { rc = -1; goto cleanup; }

        if (buf_used + bytes + 1 > buf_size) {
            buf_size = buf_used + bytes + 1;
            char *newbuf = realloc(buf, buf_size);
            if (!newbuf) {
                syslog(LOG_ERR, "realloc failed");
                rc = -1; goto cleanup;
            }
            buf = newbuf;
        }
        memcpy(buf + buf_used, tmp, bytes);
        buf_used += bytes;
        buf[buf_used] = '\0';

        if (memchr(tmp, '\n', bytes)) break;
    }

    /* Append to file */
    FILE *f = fopen(DATAFILE, "a");
    if (!f) {
        syslog(LOG_ERR, "fopen failed: %s", strerror(errno));
        rc = -1; goto cleanup;
    }
    fwrite(buf, 1, buf_used, f);
    fclose(f);

    /* Send full file back */
    f = fopen(DATAFILE, "r");
    if (!f) {
        syslog(LOG_ERR, "fopen read failed: %s", strerror(errno));
        rc = -1; goto cleanup;
    }
    while ((bytes = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (send(cfd, tmp, bytes, 0) < 0) {
            fclose(f);
            rc = -1; goto cleanup;
        }
    }
    fclose(f);

cleanup:
    free(buf);
    return rc;
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

    while (!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (caught_signal) break;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        handle_connection(client_fd, client_ip);

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(client_fd);
        client_fd = -1;
    }

    close(server_fd);
    server_fd = -1;
    remove(DATAFILE);
    closelog();
    return 0;
}
