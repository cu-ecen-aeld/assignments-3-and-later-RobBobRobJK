#include "systemcalls.h"
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

bool do_system(const char *cmd)
{
    if (cmd == NULL)
        return false;
    int ret = system(cmd);
    if (ret == -1)
        return false;
    return WIFEXITED(ret) && (WEXITSTATUS(ret) == 0);
}

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char *command[count + 1];
    for (int i = 0; i < count; i++)
        command[i] = va_arg(args, char *);
    command[count] = NULL;
    va_end(args);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1)
        return false;
    if (pid == 0) {
        execv(command[0], command);
        exit(1);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1)
        return false;
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char *command[count + 1];
    for (int i = 0; i < count; i++)
        command[i] = va_arg(args, char *);
    command[count] = NULL;
    va_end(args);

    int fd = open(outputfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        return false;

    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        close(fd);
        return false;
    }
    if (pid == 0) {
        if (dup2(fd, STDOUT_FILENO) == -1)
            exit(1);
        close(fd);
        execv(command[0], command);
        exit(1);
    }
    close(fd);
    int status;
    if (waitpid(pid, &status, 0) == -1)
        return false;
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}
