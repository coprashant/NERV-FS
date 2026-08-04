#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>
#include <limits.h>

#include "handlers.h"
#include "protocol.h"
#include "fileops.h"

/* small big endian helpers for reading and writing payload fields */

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

/* sends a full header plus optional payload to the client */
static void send_message(int client_fd, uint16_t opcode, const unsigned char *payload, uint32_t payload_len)
{
    nervfs_header_t header;
    header.magic = NERVFS_MAGIC;
    header.version = NERVFS_VERSION;
    header.opcode = opcode;
    header.payload_len = payload_len;
    header.reserved = 0;

    unsigned char header_buf[NERVFS_HEADER_SIZE];
    serialize_header(&header, header_buf);

    send_full(client_fd, header_buf, NERVFS_HEADER_SIZE);
    if (payload_len > 0 && payload != NULL) {
        send_full(client_fd, payload, payload_len);
    }
}

static void send_ok(int client_fd)
{
    send_message(client_fd, OP_RESP_OK, NULL, 0);
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

    send_message(client_fd, OP_RESP_ERR, buf, payload_len);
    free(buf);
}

static void handle_upload(int client_fd, const unsigned char *payload, uint32_t payload_len)
{
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

    /* name_len is now bounded by safe_resolve_path, safe to copy into a fixed buffer */
    char name_key[NERVFS_MAX_NAME_LEN + 1];
    memcpy(name_key, name, name_len);
    name_key[name_len] = '\0';

    /* uploads are exclusive writers, both in process and against other processes on the fd */
    fileops_lock_acquire_write(name_key);

    int fd = fileops_open_for_write(path);
    if (fd < 0) {
        fileops_lock_release_write(name_key);
        send_error(client_fd, ERR_INTERNAL, "failed to open file for writing");
        return;
    }

    if (fileops_lock_fd(fd, NERVFS_LOCK_WRITE) < 0) {
        close(fd);
        fileops_lock_release_write(name_key);
        send_error(client_fd, ERR_INTERNAL, "failed to lock file");
        return;
    }

    int write_rc = fileops_write_fd(fd, payload + offset, file_size);

    fileops_unlock_fd(fd);
    close(fd);
    fileops_lock_release_write(name_key);

    if (write_rc < 0) {
        send_error(client_fd, ERR_INTERNAL, "failed to write file");
        return;
    }

    send_ok(client_fd);
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

    char name_key[NERVFS_MAX_NAME_LEN + 1];
    memcpy(name_key, payload + 2, name_len);
    name_key[name_len] = '\0';

    /* downloads are shared readers, they only block while a writer currently holds the file */
    fileops_lock_acquire_read(name_key);

    void *mapped = NULL;
    uint64_t size = 0;
    int file_fd = -1;

    int rc = fileops_open_for_mmap_read(path, &mapped, &size, &file_fd);
    if (rc == -1) {
        fileops_lock_release_read(name_key);
        send_error(client_fd, ERR_NOT_FOUND, "file not found");
        return;
    }
    if (rc == -2) {
        fileops_lock_release_read(name_key);
        send_error(client_fd, ERR_INTERNAL, "failed to map file");
        return;
    }

    if (fileops_lock_fd(file_fd, NERVFS_LOCK_READ) < 0) {
        fileops_close_mmap(mapped, size, file_fd);
        fileops_lock_release_read(name_key);
        send_error(client_fd, ERR_INTERNAL, "failed to lock file");
        return;
    }

    /* mapping and locking both succeeded before any bytes went out, header below is safe to promise */
    nervfs_header_t header;
    header.magic = NERVFS_MAGIC;
    header.version = NERVFS_VERSION;
    header.opcode = OP_RESP_FILE_DATA;
    header.payload_len = (uint32_t)(8 + size);
    header.reserved = 0;

    unsigned char header_buf[NERVFS_HEADER_SIZE];
    serialize_header(&header, header_buf);
    send_full(client_fd, header_buf, NERVFS_HEADER_SIZE);

    unsigned char size_buf[8];
    write_u64(size_buf, size);
    send_full(client_fd, size_buf, 8);

    if (size > 0) {
        /* zero copy path, streams straight from the mapped page cache region to the socket */
        send_full(client_fd, mapped, size);
    }

    fileops_unlock_fd(file_fd);
    fileops_close_mmap(mapped, size, file_fd);
    fileops_lock_release_read(name_key);
}

static void handle_list(int client_fd)
{
    nervfs_dirent_t *entries = NULL;
    uint32_t count = 0;

    if (fileops_list_dir(&entries, &count) < 0) {
        send_error(client_fd, ERR_INTERNAL, "failed to list storage directory");
        return;
    }

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

    send_message(client_fd, OP_RESP_LIST_DATA, buf, payload_len);

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

    char name_key[NERVFS_MAX_NAME_LEN + 1];
    memcpy(name_key, payload + 2, name_len);
    name_key[name_len] = '\0';

    /* delete is a destructive mutation, treated the same as a writer for locking purposes */
    fileops_lock_acquire_write(name_key);

    int rc = fileops_delete_file(path);

    fileops_lock_release_write(name_key);

    if (rc < 0) {
        send_error(client_fd, ERR_NOT_FOUND, "file not found");
        return;
    }

    send_ok(client_fd);
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

    if (fileops_chmod_file(path, mode) < 0) {
        send_error(client_fd, ERR_NOT_FOUND, "file not found");
        return;
    }

    send_ok(client_fd);
}

void dispatch_request(int client_fd, const nervfs_header_t *header, const unsigned char *payload)
{
    switch (header->opcode) {
        case OP_REQ_UPLOAD:
            handle_upload(client_fd, payload, header->payload_len);
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