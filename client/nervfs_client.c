#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <endian.h>

#include "protocol.h"

static uint16_t read_u16(const unsigned char *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return ntohs(v);
}

static uint64_t read_u64(const unsigned char *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return be64toh(v);
}

static void write_u16(unsigned char *p, uint16_t v)
{
    uint16_t n = htons(v);
    memcpy(p, &n, 2);
}

static void write_u32(unsigned char *p, uint32_t v)
{
    uint32_t n = htonl(v);
    memcpy(p, &n, 4);
}

static void write_u64(unsigned char *p, uint64_t v)
{
    uint64_t n = htobe64(v);
    memcpy(p, &n, 8);
}

static int connect_to_server(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid host address\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static int send_request(int fd, uint16_t opcode, const unsigned char *payload, uint32_t payload_len)
{
    nervfs_header_t header;
    header.magic = NERVFS_MAGIC;
    header.version = NERVFS_VERSION;
    header.opcode = opcode;
    header.payload_len = payload_len;
    header.reserved = 0;

    unsigned char header_buf[NERVFS_HEADER_SIZE];
    serialize_header(&header, header_buf);

    if (send_full(fd, header_buf, NERVFS_HEADER_SIZE) < 0) {
        perror("send header");
        return -1;
    }

    if (payload_len > 0 && send_full(fd, payload, payload_len) < 0) {
        perror("send payload");
        return -1;
    }

    return 0;
}

static int read_response_header(int fd, nervfs_header_t *out)
{
    unsigned char header_buf[NERVFS_HEADER_SIZE];
    ssize_t got = recv_full(fd, header_buf, NERVFS_HEADER_SIZE);
    if (got != NERVFS_HEADER_SIZE) {
        fprintf(stderr, "no full response header from server\n");
        return -1;
    }

    if (parse_header(header_buf, out) < 0) {
        fprintf(stderr, "response header failed magic or version check\n");
        return -1;
    }

    return 0;
}

static int print_simple_result(int fd, const nervfs_header_t *resp)
{
    if (resp->opcode == OP_RESP_OK) {
        if (resp->payload_len == 24) {
            unsigned char buf[24];
            if (recv_full(fd, buf, 24) == 24) {
                uint64_t net_us = read_u64(buf);
                uint64_t write_us = read_u64(buf + 8);
                uint64_t sync_us = read_u64(buf + 16);
                printf("ok (total %.3f ms | net %.3f ms | write %.3f ms | sync %.3f ms)\n",
                       resp->reserved / 1000.0, net_us / 1000.0, write_us / 1000.0, sync_us / 1000.0);
                return 0;
            }
        }

        printf("ok (%.3f ms)\n", resp->reserved / 1000.0);
        return 0;
    }

    if (resp->opcode == OP_RESP_ERR) {
        unsigned char *buf = malloc(resp->payload_len);
        if (buf == NULL || recv_full(fd, buf, resp->payload_len) != (ssize_t)resp->payload_len) {
            fprintf(stderr, "error response but body could not be read\n");
            free(buf);
            return -1;
        }

        uint16_t err_code = read_u16(buf);
        uint16_t msg_len = read_u16(buf + 2);
        printf("error code %u message %.*s\n", err_code, msg_len, buf + 4);
        free(buf);
        return -1;
    }

    fprintf(stderr, "unexpected response opcode %u\n", resp->opcode);
    return -1;
}

static int do_list(int fd)
{
    if (send_request(fd, OP_REQ_LIST, NULL, 0) < 0) {
        return 1;
    }

    nervfs_header_t resp;
    if (read_response_header(fd, &resp) < 0) {
        return 1;
    }

    if (resp.opcode != OP_RESP_LIST_DATA) {
        return print_simple_result(fd, &resp) < 0 ? 1 : 0;
    }

    unsigned char *buf = malloc(resp.payload_len);
    if (buf == NULL || recv_full(fd, buf, resp.payload_len) != (ssize_t)resp.payload_len) {
        fprintf(stderr, "list body could not be read\n");
        free(buf);
        return 1;
    }

    uint32_t off = 0;
    uint32_t count = (uint32_t)((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
    off += 4;

    printf("%u file(s) (%.3f ms)\n", count, resp.reserved / 1000.0);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t nlen = read_u16(buf + off);
        off += 2;
        char name[256];
        memcpy(name, buf + off, nlen);
        name[nlen] = '\0';
        off += nlen;
        uint64_t size = read_u64(buf + off);
        off += 8;
        uint32_t mode = (uint32_t)((buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | buf[off + 3]);
        off += 4;
        printf("  %-30s %10llu bytes mode %o\n", name, (unsigned long long)size, mode);
    }

    free(buf);
    return 0;
}

static int do_upload(int fd, const char *local_file, const char *remote_name)
{
    FILE *f = fopen(local_file, "rb");
    if (f == NULL) {
        perror("fopen");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fprintf(stderr, "could not determine file size\n");
        fclose(f);
        return 1;
    }

    uint16_t name_len = (uint16_t)strlen(remote_name);
    uint32_t payload_len = 2 + name_len + 8 + (uint32_t)size;

    unsigned char *payload = malloc(payload_len);
    if (payload == NULL) {
        fclose(f);
        return 1;
    }

    write_u16(payload, name_len);
    memcpy(payload + 2, remote_name, name_len);
    write_u64(payload + 2 + name_len, (uint64_t)size);

    if (size > 0 && fread(payload + 2 + name_len + 8, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "short read on local file\n");
        fclose(f);
        free(payload);
        return 1;
    }
    fclose(f);

    int rc = send_request(fd, OP_REQ_UPLOAD, payload, payload_len);
    free(payload);
    if (rc < 0) {
        return 1;
    }

    nervfs_header_t resp;
    if (read_response_header(fd, &resp) < 0) {
        return 1;
    }

    return print_simple_result(fd, &resp) < 0 ? 1 : 0;
}

static int do_download(int fd, const char *remote_name, const char *local_file)
{
    uint16_t name_len = (uint16_t)strlen(remote_name);
    unsigned char *payload = malloc(2 + name_len);
    if (payload == NULL) {
        return 1;
    }

    write_u16(payload, name_len);
    memcpy(payload + 2, remote_name, name_len);

    int rc = send_request(fd, OP_REQ_DOWNLOAD, payload, 2 + name_len);
    free(payload);
    if (rc < 0) {
        return 1;
    }

    nervfs_header_t resp;
    if (read_response_header(fd, &resp) < 0) {
        return 1;
    }

    if (resp.opcode != OP_RESP_FILE_DATA) {
        return print_simple_result(fd, &resp) < 0 ? 1 : 0;
    }

    unsigned char size_buf[8];
    if (recv_full(fd, size_buf, 8) != 8) {
        fprintf(stderr, "could not read file size prefix\n");
        return 1;
    }
    uint64_t size = read_u64(size_buf);

    unsigned char *data = NULL;
    if (size > 0) {
        data = malloc(size);
        if (data == NULL || recv_full(fd, data, size) != (ssize_t)size) {
            fprintf(stderr, "could not read full file body\n");
            free(data);
            return 1;
        }
    }

    FILE *out = fopen(local_file, "wb");
    if (out == NULL) {
        perror("fopen");
        free(data);
        return 1;
    }

    if (size > 0) {
        fwrite(data, 1, size, out);
    }
    fclose(out);
    free(data);

    printf("downloaded %llu bytes to %s (server prep %.3f ms)\n",
           (unsigned long long)size, local_file, resp.reserved / 1000.0);
    return 0;
}

static int do_delete(int fd, const char *remote_name)
{
    uint16_t name_len = (uint16_t)strlen(remote_name);
    unsigned char *payload = malloc(2 + name_len);
    if (payload == NULL) {
        return 1;
    }

    write_u16(payload, name_len);
    memcpy(payload + 2, remote_name, name_len);

    int rc = send_request(fd, OP_REQ_DELETE, payload, 2 + name_len);
    free(payload);
    if (rc < 0) {
        return 1;
    }

    nervfs_header_t resp;
    if (read_response_header(fd, &resp) < 0) {
        return 1;
    }

    return print_simple_result(fd, &resp) < 0 ? 1 : 0;
}

static int do_chmod(int fd, const char *remote_name, const char *mode_str)
{
    uint32_t mode = (uint32_t)strtol(mode_str, NULL, 8);
    uint16_t name_len = (uint16_t)strlen(remote_name);

    unsigned char *payload = malloc(2 + name_len + 4);
    if (payload == NULL) {
        return 1;
    }

    write_u16(payload, name_len);
    memcpy(payload + 2, remote_name, name_len);
    write_u32(payload + 2 + name_len, mode);

    int rc = send_request(fd, OP_REQ_CHMOD, payload, 2 + name_len + 4);
    free(payload);
    if (rc < 0) {
        return 1;
    }

    nervfs_header_t resp;
    if (read_response_header(fd, &resp) < 0) {
        return 1;
    }

    return print_simple_result(fd, &resp) < 0 ? 1 : 0;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "usage %s host port command args\n", argv[0]);
        fprintf(stderr, "  list\n");
        fprintf(stderr, "  upload local_file remote_name\n");
        fprintf(stderr, "  download remote_name local_file\n");
        fprintf(stderr, "  delete remote_name\n");
        fprintf(stderr, "  chmod remote_name mode_octal\n");
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *command = argv[3];

    int fd = connect_to_server(host, port);
    if (fd < 0) {
        return 1;
    }

    int rc = 1;

    if (strcmp(command, "list") == 0) {
        rc = do_list(fd);
    } else if (strcmp(command, "upload") == 0 && argc == 6) {
        rc = do_upload(fd, argv[4], argv[5]);
    } else if (strcmp(command, "download") == 0 && argc == 6) {
        rc = do_download(fd, argv[4], argv[5]);
    } else if (strcmp(command, "delete") == 0 && argc == 5) {
        rc = do_delete(fd, argv[4]);
    } else if (strcmp(command, "chmod") == 0 && argc == 6) {
        rc = do_chmod(fd, argv[4], argv[5]);
    } else {
        fprintf(stderr, "unknown command or wrong number of arguments\n");
    }

    close(fd);
    return rc;
}