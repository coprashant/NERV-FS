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

/* reads path fully into a malloc buffer, caller must free it, returns 0 or negative */
int fileops_read_file(const char *path, unsigned char **out_data, uint64_t *out_size);

/* opens path and mmaps it read only, returns 0 on success */
/* returns negative one if the file could not be opened or stat failed */
/* returns negative two if mmap itself failed */
/* a zero length file yields out_map NULL and out_size zero, caller must still close out_fd */
int fileops_open_for_mmap_read(const char *path, void **out_map, uint64_t *out_size, int *out_fd);

/* unmaps a region from fileops_open_for_mmap_read and closes its fd */
void fileops_close_mmap(void *map, uint64_t size, int fd);

/* removes path, returns 0 or negative */
int fileops_delete_file(const char *path);

/* changes permission bits on path, returns 0 or negative */
int fileops_chmod_file(const char *path, uint32_t mode);

/* lists regular files directly inside the storage root, caller frees out_entries */
int fileops_list_dir(nervfs_dirent_t **out_entries, uint32_t *out_count);

#endif