#ifndef NERVFS_FILEOPS_H
#define NERVFS_FILEOPS_H

#include <stdint.h>
#include <stddef.h>

#define NERVFS_MAX_NAME_LEN 255

/* one entry returned by fileops_list_dir */
typedef struct {
    char name[NERVFS_MAX_NAME_LEN + 1];
    uint64_t size;
    uint32_t mode;
} nervfs_dirent_t;

/* resolves and creates the storage root, call once at startup, returns 0 or negative */
int fileops_init(const char *root_dir);

/* validates a client supplied name and writes the safe absolute path into out_path */
/* rejects slashes dot names oversize names and anything outside the storage root */
int safe_resolve_path(const char *name, uint16_t name_len, char *out_path, size_t out_size);

/* creates or overwrites path with size bytes from data, returns 0 or negative */
int fileops_write_file(const char *path, const unsigned char *data, uint64_t size);

/* writes size bytes from data to an already open fd, returns 0 or negative */
int fileops_write_fd(int fd, const unsigned char *data, uint64_t size);

/* opens path for writing truncating any existing content, returns fd or negative */
int fileops_open_for_write(const char *path);

/* reads path fully into a malloc buffer, caller must free it, returns 0 or negative */
int fileops_read_file(const char *path, unsigned char **out_data, uint64_t *out_size);

/* opens path and mmaps it read only, returns 0 on success */
/* returns negative one if the file could not be opened or stat failed */
/* returns negative two if mmap itself failed */
/* a zero length file yields out_map NULL and out_size zero, caller must still close out_fd */
int fileops_open_for_mmap_read(const char *path, void **out_map, uint64_t *out_size, int *out_fd);

/* unmaps a region from fileops_open_for_mmap_read and closes its fd */
void fileops_close_mmap(void *map, uint64_t size, int fd);

/* which kind of fcntl advisory lock to place on a file descriptor */
typedef enum {
    NERVFS_LOCK_READ,
    NERVFS_LOCK_WRITE
} nervfs_lock_kind_t;

/* places a blocking fcntl advisory lock on the whole file, cross process safety layer */
int fileops_lock_fd(int fd, nervfs_lock_kind_t kind);

/* releases a fcntl advisory lock placed by fileops_lock_fd */
int fileops_unlock_fd(int fd);

/* in process locking policy keyed by filename */
/* uploads and deletes are exclusive writers, downloads are shared readers */
/* a pending writer blocks new readers and other writers until it releases */
void fileops_lock_acquire_write(const char *name);
void fileops_lock_release_write(const char *name);
void fileops_lock_acquire_read(const char *name);
void fileops_lock_release_read(const char *name);

/* removes path, returns 0 or negative */
int fileops_delete_file(const char *path);

/* changes permission bits on path, returns 0 or negative */
int fileops_chmod_file(const char *path, uint32_t mode);

/* lists regular files directly inside the storage root, caller frees out_entries */
int fileops_list_dir(nervfs_dirent_t **out_entries, uint32_t *out_count);

#endif