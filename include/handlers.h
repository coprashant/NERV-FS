#ifndef NERVFS_HANDLERS_H
#define NERVFS_HANDLERS_H

#include "protocol.h"

/* looks at the opcode in header, runs the matching file operation, sends the response itself */
void dispatch_request(int client_fd, const nervfs_header_t *header, const unsigned char *payload);

#endif