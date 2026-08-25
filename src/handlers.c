#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <endian.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "handlers.h"
#include "protocol.h"
#include "fileops.h"

static uint16_t read_u16(const unsigned char *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return ntohs(v);
}

static uint32_t read_u32(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
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

static uint64_t elapsed_microseconds(const struct timespec *start, const struct timespec *end)
{
    int64_t sec_diff = (int64_t)(end->tv_sec - start->tv_sec);
    int64_t nsec_diff = (int64_t)(end->tv_nsec - start->tv_nsec);

    if (nsec_diff < 0) {
        sec_diff -= 1;
        nsec_diff += 1000000000L;
    }

    return (uint64_t)(sec_diff * 1000000L + nsec_diff / 1000);
}

static void send_message(int client_fd, uint16_t opcode, const unsigned char *payload,
                          uint32_t payload_len, uint64_t elapsed_us)
{
    nervfs_header_t header;
    header.magic = NERVFS_MAGIC;
    header.version = NERVFS_VERSION;
    header.opcode = opcode;
    header.payload_len = payload_len;
    header.reserved = elapsed_us;

    unsigned char header_buf[NERVFS_HEADER_SIZE];
    serialize_header(&header, header_buf);

    send_full(client_fd, header_buf, NERVFS_HEADER_SIZE);
    if (payload_len > 0 && payload != NULL) {
        send_full(client_fd, payload, payload_len);
    }
}

static void send_ok(int client_fd, uint64_t elapsed_us)
{
    send_message(client_fd, OP_RESP_OK, NULL, 0, elapsed_us);
}

static void send_error(int client_fd, uint16_t err_code, const char *msg)
{
    uint16_t msg_len = (uint16_t)strlen(msg);
    uint32_t payload_len = 2 + 2 + msg_len;

    unsigned char *buf = malloc(payload_len);
    if (buf == NULL) {
        return;
    }

    write_u16(buf, err_code);
    write_u16(buf + 2, msg_len);
    memcpy(buf + 4, msg, msg_len);

    send_message(client_fd, OP_RESP_ERR, buf, payload_len, 0);
    free(buf);
}

static void handle_upload_timed(int client_fd, const unsigned char *payload, uint32_t payload_len, struct timespec net_start)
{
    struct timespec net_end, write_start, write_end, sync_end;

    /* Measure socket network read time */
    clock_gettime(CLOCK_MONOTONIC, &net_end);
    uint64_t network_us = elapsed_microseconds(&net_start, &net_end);

    if (payload_len < 2) {
        send_error(client_fd, ERR_BAD_REQUEST, "upload payload too short");
        return;
    }

    uint16_t name_len = read_u16(payload);
    uint32_t offset = 2;

    if (payload_len < offset + (uint32_t)name_len + 8) {
        send_error(client_fd, ERR_BAD_REQUEST, "upload payload truncated");
        return;
    }

    const unsigned char *name = payload + offset;
    offset += name_len;

    uint64_t file_size = read_u64(payload + offset);
    offset += 8;

    if ((uint64_t)(payload_len - offset) != file_size) {
        send_error(client_fd, ERR_BAD_REQUEST, "declared file size does not match payload");
        return;
    }

    char path[PATH_MAX];
    if (safe_resolve_path((const char *)name, name_len, path, sizeof(path)) < 0) {
        send_error(client_fd, ERR_FORBIDDEN, "invalid filename");
        return;
    }

    /* Measure RAM page cache write time */
    clock_gettime(CLOCK_MONOTONIC, &write_start);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        send_error(client_fd, ERR_INTERNAL, "failed to open file");
        return;
    }

    ssize_t bytes_written = write(fd, payload + offset, file_size);
    clock_gettime(CLOCK_MONOTONIC, &write_end);
    uint64_t write_us = elapsed_microseconds(&write_start, &write_end);

    if (bytes_written < 0 || (size_t)bytes_written != file_size) {
        close(fd);
        send_error(client_fd, ERR_INTERNAL, "failed to write file body");
        return;
    }

    /* Measure physical disk sync time */
    fsync(fd);
    clock_gettime(CLOCK_MONOTONIC, &sync_end);
    close(fd);
    uint64_t sync_us = elapsed_microseconds(&write_end, &sync_end);

    /* Send metrics breakdown payload */
    unsigned char metrics_buf[24];
    write_u64(metrics_buf + 0, network_us);
    write_u64(metrics_buf + 8, write_us);
    write_u64(metrics_buf + 16, sync_us);

    uint64_t total_us = network_us + write_us + sync_us;
    send_message(client_fd, OP_RESP_OK, metrics_buf, sizeof(metrics_buf), total_us);
}

static void handle_download(int client_fd, const unsigned char *payload, uint32_t payload_len)
{
    if (payload_len < 2) {
        send_error(client_fd, ERR_BAD_REQUEST, "download payload too short");
        return;
    }

    uint16_t name_len = read_u16(payload);
    if (payload_len != 2 + (uint32_t)name_len) {
        send_error(client_fd, ERR_BAD_REQUEST, "download payload malformed");
        return;
    }

    char path[PATH_MAX];
    if (safe_resolve_path((const char *)(payload + 2), name_len, path, sizeof(path)) < 0) {
        send_error(client_fd, ERR_FORBIDDEN, "invalid filename");
        return;
    }

    void *mapped = NULL;
    uint64_t size = 0;
    int file_fd = -1;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = fileops_open_for_mmap_read(path, &mapped, &size, &file_fd);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (rc == -1) {
        send_error(client_fd, ERR_NOT_FOUND, "file not found");
        return;
    }
    if (rc == -2) {
        send_error(client_fd, ERR_INTERNAL, "failed to map file");
        return;
    }

    uint64_t elapsed_us = elapsed_microseconds(&start, &end);

    nervfs_header_t header;
    header.magic = NERVFS_MAGIC;
    header.version = NERVFS_VERSION;
    header.opcode = OP_RESP_FILE_DATA;
    header.payload_len = (uint32_t)(8 + size);
    header.reserved = elapsed_us;

    unsigned char header_buf[NERVFS_HEADER_SIZE];
    serialize_header(&header, header_buf);
    send_full(client_fd, header_buf, NERVFS_HEADER_SIZE);

    unsigned char size_buf[8];
    write_u64(size_buf, size);
    send_full(client_fd, size_buf, 8);

    if (size > 0) {
        send_full(client_fd, mapped, size);
    }

    fileops_close_mmap(mapped, size, file_fd);
}

static void handle_list(int client_fd)
{
    nervfs_dirent_t *entries = NULL;
    uint32_t count = 0;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int list_rc = fileops_list_dir(&entries, &count);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (list_rc < 0) {
        send_error(client_fd, ERR_INTERNAL, "failed to list storage directory");
        return;
    }

    uint64_t elapsed_us = elapsed_microseconds(&start, &end);

    uint32_t payload_len = 4;
    for (uint32_t i = 0; i < count; i++) {
        payload_len += 2 + (uint32_t)strlen(entries[i].name) + 8 + 4;
    }

    unsigned char *buf = malloc(payload_len);
    if (buf == NULL) {
        send_error(client_fd, ERR_INTERNAL, "response allocation failed");
        free(entries);
        return;
    }

    uint32_t off = 0;
    write_u32(buf + off, count);
    off += 4;

    for (uint32_t i = 0; i < count; i++) {
        uint16_t nlen = (uint16_t)strlen(entries[i].name);
        write_u16(buf + off, nlen);
        off += 2;
        memcpy(buf + off, entries[i].name, nlen);
        off += nlen;
        write_u64(buf + off, entries[i].size);
        off += 8;
        write_u32(buf + off, entries[i].mode);
        off += 4;
    }

    send_message(client_fd, OP_RESP_LIST_DATA, buf, payload_len, elapsed_us);

    free(buf);
    free(entries);
}

static void handle_delete(int client_fd, const unsigned char *payload, uint32_t payload_len)
{
    if (payload_len < 2) {
        send_error(client_fd, ERR_BAD_REQUEST, "delete payload too short");
        return;
    }

    uint16_t name_len = read_u16(payload);
    if (payload_len != 2 + (uint32_t)name_len) {
        send_error(client_fd, ERR_BAD_REQUEST, "delete payload malformed");
        return;
    }

    char path[PATH_MAX];
    if (safe_resolve_path((const char *)(payload + 2), name_len, path, sizeof(path)) < 0) {
        send_error(client_fd, ERR_FORBIDDEN, "invalid filename");
        return;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int delete_rc = fileops_delete_file(path);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (delete_rc < 0) {
        send_error(client_fd, ERR_NOT_FOUND, "file not found");
        return;
    }

    send_ok(client_fd, elapsed_microseconds(&start, &end));
}

static void handle_chmod(int client_fd, const unsigned char *payload, uint32_t payload_len)
{
    if (payload_len < 2) {
        send_error(client_fd, ERR_BAD_REQUEST, "chmod payload too short");
        return;
    }

    uint16_t name_len = read_u16(payload);
    if (payload_len != 2 + (uint32_t)name_len + 4) {
        send_error(client_fd, ERR_BAD_REQUEST, "chmod payload malformed");
        return;
    }

    char path[PATH_MAX];
    if (safe_resolve_path((const char *)(payload + 2), name_len, path, sizeof(path)) < 0) {
        send_error(client_fd, ERR_FORBIDDEN, "invalid filename");
        return;
    }

    uint32_t mode = read_u32(payload + 2 + name_len);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int chmod_rc = fileops_chmod_file(path, mode);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (chmod_rc < 0) {
        send_error(client_fd, ERR_NOT_FOUND, "file not found");
        return;
    }

    send_ok(client_fd, elapsed_microseconds(&start, &end));
}

void dispatch_request_timed(int client_fd, const nervfs_header_t *header, const unsigned char *payload, struct timespec net_start)
{
    switch (header->opcode) {
        case OP_REQ_UPLOAD:
            handle_upload_timed(client_fd, payload, header->payload_len, net_start);
            break;
        case OP_REQ_DOWNLOAD:
            handle_download(client_fd, payload, header->payload_len);
            break;
        case OP_REQ_LIST:
            handle_list(client_fd);
            break;
        case OP_REQ_DELETE:
            handle_delete(client_fd, payload, header->payload_len);
            break;
        case OP_REQ_CHMOD:
            handle_chmod(client_fd, payload, header->payload_len);
            break;
        default:
            send_error(client_fd, ERR_BAD_REQUEST, "unknown opcode");
            break;
    }
}