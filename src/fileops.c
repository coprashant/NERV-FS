#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "fileops.h"
#include "logging.h"

/* resolved absolute path of the storage root, set once by fileops_init */
static char storage_root_real[PATH_MAX];

int fileops_init(const char *root_dir)
{
    struct stat st;
    if (stat(root_dir, &st) < 0) {
        if (mkdir(root_dir, 0755) < 0) {
            log_error("failed to create storage root");
            return -1;
        }
    }

    if (realpath(root_dir, storage_root_real) == NULL) {
        log_error("failed to resolve storage root path");
        return -1;
    }

    return 0;
}

int safe_resolve_path(const char *name, uint16_t name_len, char *out_path, size_t out_size)
{
    if (name_len == 0 || name_len > NERVFS_MAX_NAME_LEN) {
        return -1;
    }

    char safe_name[NERVFS_MAX_NAME_LEN + 1];
    memcpy(safe_name, name, name_len);
    safe_name[name_len] = '\0';

    /* if a null byte was smuggled inside name_len bytes the string looks shorter than claimed */
    if (strlen(safe_name) != name_len) {
        return -1;
    }

    /* flat storage directory, no subfolders, so any slash is a rejection not a path */
    if (strchr(safe_name, '/') != NULL) {
        return -1;
    }

    if (strcmp(safe_name, ".") == 0 || strcmp(safe_name, "..") == 0) {
        return -1;
    }

    int written = snprintf(out_path, out_size, "%s/%s", storage_root_real, safe_name);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

int fileops_write_file(const char *path, const unsigned char *data, uint64_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("open for write failed");
        return -1;
    }

    uint64_t total = 0;
    while (total < size) {
        ssize_t n = write(fd, data + total, size - total);
        if (n < 0) {
            log_error("write failed");
            close(fd);
            return -1;
        }
        total += (uint64_t)n;
    }

    close(fd);
    return 0;
}

int fileops_read_file(const char *path, unsigned char **out_data, uint64_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("fstat failed");
        close(fd);
        return -1;
    }

    uint64_t size = (uint64_t)st.st_size;
    unsigned char *buf = NULL;

    if (size > 0) {
        buf = malloc(size);
        if (buf == NULL) {
            log_error("read buffer allocation failed");
            close(fd);
            return -1;
        }

        uint64_t total = 0;
        while (total < size) {
            ssize_t n = read(fd, buf + total, size - total);
            if (n <= 0) {
                log_error("read failed");
                free(buf);
                close(fd);
                return -1;
            }
            total += (uint64_t)n;
        }
    }

    close(fd);
    *out_data = buf;
    *out_size = size;
    return 0;
}

int fileops_delete_file(const char *path)
{
    if (unlink(path) < 0) {
        return -1;
    }
    return 0;
}

int fileops_chmod_file(const char *path, uint32_t mode)
{
    if (chmod(path, (mode_t)mode) < 0) {
        return -1;
    }
    return 0;
}

int fileops_list_dir(nervfs_dirent_t **out_entries, uint32_t *out_count)
{
    DIR *dir = opendir(storage_root_real);
    if (dir == NULL) {
        log_error("opendir failed");
        return -1;
    }

    size_t capacity = 16;
    uint32_t count = 0;
    nervfs_dirent_t *entries = malloc(capacity * sizeof(nervfs_dirent_t));
    if (entries == NULL) {
        closedir(dir);
        return -1;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", storage_root_real, de->d_name);
        if (written < 0 || (size_t)written >= sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) < 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (count == capacity) {
            capacity *= 2;
            nervfs_dirent_t *grown = realloc(entries, capacity * sizeof(nervfs_dirent_t));
            if (grown == NULL) {
                free(entries);
                closedir(dir);
                return -1;
            }
            entries = grown;
        }

        strncpy(entries[count].name, de->d_name, NERVFS_MAX_NAME_LEN);
        entries[count].name[NERVFS_MAX_NAME_LEN] = '\0';
        entries[count].size = (uint64_t)st.st_size;
        entries[count].mode = (uint32_t)(st.st_mode & 07777);
        count++;
    }

    closedir(dir);
    *out_entries = entries;
    *out_count = count;
    return 0;
}