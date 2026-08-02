/* minimal CLI client for testing the wire protocol */
/* connects to the server builds a header and payload for a chosen opcode */
/* sends it then reads and prints the response */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "usage %s host port command\n", argv[0]);
        fprintf(stderr, "commands list\n");
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *command = argv[3];

    uint16_t opcode;
    if (strcmp(command, "list") == 0) {
        opcode = OP_REQ_LIST;
    } else {
        fprintf(stderr, "command not implemented yet in this phase\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid host address\n");
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    nervfs_header_t header;
    header.magic = NERVFS_MAGIC;
    header.version = NERVFS_VERSION;
    header.opcode = opcode;
    header.payload_len = 0;
    header.reserved = 0;

    unsigned char header_buf[NERVFS_HEADER_SIZE];
    serialize_header(&header, header_buf);

    if (send_full(fd, header_buf, NERVFS_HEADER_SIZE) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    printf("request sent opcode %s\n", opcode_name(opcode));

    close(fd);
    return 0;
}
