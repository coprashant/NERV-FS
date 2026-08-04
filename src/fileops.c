#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>

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

int fileops_write_fd(int fd, const unsigned char *data, uint64_t size)
{
    uint64_t total = 0;
    while (total < size) {
        ssize_t n = write(fd, data + total, size - total);
        if (n < 0) {
            log_error("write failed");
            return -1;
        }
        total += (uint64_t)n;
    }

    return 0;
}

int fileops_open_for_write(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("open for write failed");
        return -1;
    }
    return fd;
}

int fileops_write_file(const char *path, const unsigned char *data, uint64_t size)
{
    int fd = fileops_open_for_write(path);
    if (fd < 0) {
        return -1;
    }

    int rc = fileops_write_fd(fd, data, size);
    close(fd);
    return rc;
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

/* opens path read only and mmaps it, zero length files are special cased since mmap cannot map them */
int fileops_open_for_mmap_read(const char *path, void **out_map, uint64_t *out_size, int *out_fd)
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

    if (size == 0) {
        *out_map = NULL;
        *out_size = 0;
        *out_fd = fd;
        return 0;
    }

    void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        log_error("mmap failed");
        close(fd);
        return -2;
    }

    *out_map = mapped;
    *out_size = size;
    *out_fd = fd;
    return 0;
}

void fileops_close_mmap(void *map, uint64_t size, int fd)
{
    if (map != NULL && size > 0) {
        munmap(map, size);
    }
    close(fd);
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

/* cross process layer, a blocking fcntl advisory lock on the whole file */

int fileops_lock_fd(int fd, nervfs_lock_kind_t kind)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = (kind == NERVFS_LOCK_WRITE) ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        log_error("fcntl lock failed");
        return -1;
    }

    return 0;
}

int fileops_unlock_fd(int fd)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLK, &fl) < 0) {
        log_error("fcntl unlock failed");
        return -1;
    }

    return 0;
}

/* in process layer, one small table entry per distinct filename ever touched */

typedef struct file_lock_entry {
    char name[NERVFS_MAX_NAME_LEN + 1];
    int writers;
    int readers;
    struct file_lock_entry *next;
} file_lock_entry_t;

static file_lock_entry_t *lock_table_head = NULL;
static pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lock_table_cond = PTHREAD_COND_INITIALIZER;

/* caller must already hold lock_table_mutex, finds or creates the entry for name */
static file_lock_entry_t *get_entry_locked(const char *name)
{
    for (file_lock_entry_t *e = lock_table_head; e != NULL; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            return e;
        }
    }

    file_lock_entry_t *e = malloc(sizeof(file_lock_entry_t));
    if (e == NULL) {
        log_error("lock table entry allocation failed");
        return NULL;
    }

    strncpy(e->name, name, NERVFS_MAX_NAME_LEN);
    e->name[NERVFS_MAX_NAME_LEN] = '\0';
    e->writers = 0;
    e->readers = 0;
    e->next = lock_table_head;
    lock_table_head = e;
    return e;
}

void fileops_lock_acquire_write(const char *name)
{
    pthread_mutex_lock(&lock_table_mutex);

    file_lock_entry_t *e = get_entry_locked(name);
    while (e != NULL && (e->writers > 0 || e->readers > 0)) {
        pthread_cond_wait(&lock_table_cond, &lock_table_mutex);
    }
    if (e != NULL) {
        e->writers = 1;
    }

    pthread_mutex_unlock(&lock_table_mutex);
}

void fileops_lock_release_write(const char *name)
{
    pthread_mutex_lock(&lock_table_mutex);

    file_lock_entry_t *e = get_entry_locked(name);
    if (e != NULL) {
        e->writers = 0;
    }
    pthread_cond_broadcast(&lock_table_cond);

    pthread_mutex_unlock(&lock_table_mutex);
}

void fileops_lock_acquire_read(const char *name)
{
    pthread_mutex_lock(&lock_table_mutex);

    file_lock_entry_t *e = get_entry_locked(name);
    while (e != NULL && e->writers > 0) {
        pthread_cond_wait(&lock_table_cond, &lock_table_mutex);
    }
    if (e != NULL) {
        e->readers++;
    }

    pthread_mutex_unlock(&lock_table_mutex);
}

void fileops_lock_release_read(const char *name)
{
    pthread_mutex_lock(&lock_table_mutex);

    file_lock_entry_t *e = get_entry_locked(name);
    if (e != NULL && e->readers > 0) {
        e->readers--;
    }
    pthread_cond_broadcast(&lock_table_cond);

    pthread_mutex_unlock(&lock_table_mutex);
}