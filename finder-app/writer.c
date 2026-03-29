#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments: %d. Usage: writer <file> <string>", argc - 1);
        fprintf(stderr, "Usage: %s <file> <string>\n", argv[0]);
        closelog();
        return 1;
    }

    const char *filepath = argv[1];
    const char *str      = argv[2];

    FILE *f = fopen(filepath, "w");
    if (f == NULL) {
        syslog(LOG_ERR, "Failed to open file %s: %s", filepath, strerror(errno));
        fprintf(stderr, "Error opening file %s: %s\n", filepath, strerror(errno));
        closelog();
        return 1;
    }

    syslog(LOG_DEBUG, "Writing %s to %s", str, filepath);

    if (fputs(str, f) == EOF) {
        syslog(LOG_ERR, "Failed to write to file %s: %s", filepath, strerror(errno));
        fprintf(stderr, "Error writing to file %s: %s\n", filepath, strerror(errno));
        fclose(f);
        closelog();
        return 1;
    }

    fclose(f);
    closelog();
    return 0;
}
