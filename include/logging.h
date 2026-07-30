#ifndef NERVFS_LOGGING_H
#define NERVFS_LOGGING_H

/* prints an info line prefixed with INFO */
void log_info(const char *msg);

/* prints an error line prefixed with ERROR, includes errno text */
void log_error(const char *msg);

#endif
