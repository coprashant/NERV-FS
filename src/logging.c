#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "logging.h"

void log_info(const char *msg)
{
    printf("INFO %s\n", msg);
    fflush(stdout);
}

void log_error(const char *msg)
{
    fprintf(stderr, "ERROR %s reason %s\n", msg, strerror(errno));
}
