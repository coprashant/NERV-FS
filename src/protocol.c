#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <endian.h>

#include "protocol.h"

ssize_t recv_full(int fd, void *buf, size_t len)
{
    size_t total = 0;
    unsigned char *p = (unsigned char *)buf;

    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n == 0) {
            return (ssize_t)total;
        }
        if (n < 0) {
            return -1;
        }
        total += (size_t)n;
    }

    return (ssize_t)total;
}

ssize_t send_full(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    const unsigned char *p = (const unsigned char *)buf;

    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            return -1;
        }
        total += (size_t)n;
    }

    return (ssize_t)total;
}

int parse_header(const unsigned char *buf, nervfs_header_t *out)
{
    out->magic = buf[0];
    out->version = buf[1];

    uint16_t opcode_net;
    memcpy(&opcode_net, buf + 2, 2);
    out->opcode = ntohs(opcode_net);

    uint32_t len_net;
    memcpy(&len_net, buf + 4, 4);
    out->payload_len = ntohl(len_net);

    uint64_t reserved_net;
    memcpy(&reserved_net, buf + 8, 8);
    out->reserved = be64toh(reserved_net);

    if (out->magic != NERVFS_MAGIC) {
        return -1;
    }
    if (out->version != NERVFS_VERSION) {
        return -1;
    }

    return 0;
}

void serialize_header(const nervfs_header_t *in, unsigned char *buf)
{
    buf[0] = in->magic;
    buf[1] = in->version;

    uint16_t opcode_net = htons(in->opcode);
    memcpy(buf + 2, &opcode_net, 2);

    uint32_t len_net = htonl(in->payload_len);
    memcpy(buf + 4, &len_net, 4);

    uint64_t reserved_net = htobe64(in->reserved);
    memcpy(buf + 8, &reserved_net, 8);
}

const char *opcode_name(uint16_t opcode)
{
    switch (opcode) {
        case OP_REQ_UPLOAD:     return "REQ_UPLOAD";
        case OP_REQ_DOWNLOAD:   return "REQ_DOWNLOAD";
        case OP_REQ_LIST:       return "REQ_LIST";
        case OP_REQ_DELETE:     return "REQ_DELETE";
        case OP_REQ_CHMOD:      return "REQ_CHMOD";
        case OP_RESP_OK:        return "RESP_OK";
        case OP_RESP_ERR:       return "RESP_ERR";
        case OP_RESP_FILE_DATA: return "RESP_FILE_DATA";
        case OP_RESP_LIST_DATA: return "RESP_LIST_DATA";
        default:                return "UNKNOWN";
    }
}