#include <arpa/inet.h>    // For htonl/ntohl
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgit.h"

// --- Safe I/O Helpers ---
ssize_t read_all(int fd, void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t ret = read(fd, (char *) buf + total, count - total);
        if (ret == 0)
            break;    // EOF
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += ret;
    }
    return total;
}

ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t ret = write(fd, (const char *) buf + total, count - total);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += ret;
    }
    return total;
}

// --- Serialization Helper (Untouched) ---
void *serialize_snapshot(Snapshot *snap, size_t *out_len) {
    size_t total_size = sizeof(uint32_t) * 2 + 256;
    FileEntry *curr = snap->files;
    while (curr) {
        total_size += (sizeof(FileEntry) - sizeof(void *) * 2);
        if (curr->num_blocks > 0)
            total_size += (sizeof(BlockTable) * curr->num_blocks);
        curr = curr->next;
    }

    void *buf = malloc(total_size);
    void *ptr = buf;

    memcpy(ptr, &snap->snapshot_id, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(ptr, &snap->file_count, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(ptr, snap->message, 256);
    ptr += 256;

    curr = snap->files;
    while (curr) {
        size_t fixed_size = sizeof(FileEntry) - sizeof(void *) * 2;
        memcpy(ptr, curr, fixed_size);
        ptr += fixed_size;
        if (curr->num_blocks > 0) {
            size_t blocks_size = sizeof(BlockTable) * curr->num_blocks;
            memcpy(ptr, curr->chunks, blocks_size);
            ptr += blocks_size;
        }
        curr = curr->next;
    }

    *out_len = total_size;
    return buf;
}

// --- Deserialization Helper ---
Snapshot *deserialize_snapshot(void *buf, size_t len) {
    if (!buf)
        return NULL;

    Snapshot *snap = calloc(1, sizeof(Snapshot));
    uint8_t *ptr = (uint8_t *) buf;

    memcpy(&snap->snapshot_id, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(&snap->file_count, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(snap->message, ptr, 256);
    ptr += 256;

    FileEntry *last_fe = NULL;
    size_t fixed_size = sizeof(FileEntry) - (sizeof(void *) * 2);

    for (uint32_t i = 0; i < snap->file_count; i++) {
        FileEntry *fe = calloc(1, sizeof(FileEntry));

        memcpy(fe, ptr, fixed_size);
        ptr += fixed_size;

        if (fe->num_blocks > 0) {
            size_t blocks_size = sizeof(BlockTable) * fe->num_blocks;
            fe->chunks = malloc(blocks_size);
            memcpy(fe->chunks, ptr, blocks_size);
            ptr += blocks_size;
        }

        if (i == 0) {
            snap->files = fe;
        } else {
            last_fe->next = fe;
        }
        last_fe = fe;
    }
    return snap;
}

// --- Network Operations ---
void mgit_send(const char *id_str) {
    uint32_t magic = htonl(MAGIC_NUMBER);
    write_all(STDOUT_FILENO, &magic, 4);

    uint32_t id = (uint32_t) atoi(id_str);
    Snapshot *snap = load_snapshot_from_disk(id);
    if (!snap)
        exit(1);

    size_t manifest_len;
    void *manifest_buf = serialize_snapshot(snap, &manifest_len);

    uint32_t net_len = htonl((uint32_t) manifest_len);
    write_all(STDOUT_FILENO, &net_len, 4);
    write_all(STDOUT_FILENO, manifest_buf, manifest_len);

    FILE *vault = fopen(".mgit/data.bin", "rb");
    if (!vault)
        exit(1);

    FileEntry *curr = snap->files;
    while (curr) {
        if (!curr->is_directory && curr->num_blocks > 0) {
            for (int i = 0; i < curr->num_blocks; i++) {
                fseek(vault, curr->chunks[i].physical_offset, SEEK_SET);

                char *temp_buf = malloc(curr->chunks[i].compressed_size);

                if (fread(temp_buf, 1, curr->chunks[i].compressed_size, vault) ==
                    curr->chunks[i].compressed_size) {
                    write_all(STDOUT_FILENO, temp_buf, curr->chunks[i].compressed_size);
                }
                free(temp_buf);
            }
        }
        curr = curr->next;
    }

    fclose(vault);
    free(manifest_buf);
    free_snapshot(snap);
}

void mgit_receive(const char *dest_path) {
    mkdir(dest_path, 0755);
    chdir(dest_path);
    mgit_init();

    uint32_t magic;
    if (read_all(STDIN_FILENO, &magic, 4) != 4 || ntohl(magic) != MAGIC_NUMBER) {
        exit(1);
    }

    uint32_t net_len;
    if (read_all(STDIN_FILENO, &net_len, 4) != 4)
        exit(1);

    size_t manifest_len = ntohl(net_len);
    void *buf = malloc(manifest_len);

    if (read_all(STDIN_FILENO, buf, manifest_len) != manifest_len) {
        free(buf);
        exit(1);
    }

    Snapshot *snap = deserialize_snapshot(buf, manifest_len);
    free(buf);

    FILE *vault = fopen(".mgit/data.bin", "ab");
    if (!vault)
        exit(1);

    FileEntry *curr = snap->files;
    while (curr) {
        if (curr->is_directory) {
            mkdir(curr->path, 0755);
        } else if (curr->num_blocks > 0) {
            for (int i = 0; i < curr->num_blocks; i++) {
                curr->chunks[i].physical_offset = ftell(vault);

                char *chunk_data = malloc(curr->chunks[i].compressed_size);

                if (read_all(STDIN_FILENO, chunk_data, curr->chunks[i].compressed_size) ==
                    curr->chunks[i].compressed_size) {
                    fwrite(chunk_data, 1, curr->chunks[i].compressed_size, vault);
                } else {
                    free(chunk_data);
                    exit(1);
                }
                free(chunk_data);
            }
        }
        curr = curr->next;
    }

    store_snapshot_to_disk(snap);
    update_head(snap->snapshot_id);
    fclose(vault);

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%u", snap->snapshot_id);
    mgit_restore(id_str);

    free_snapshot(snap);
}

void mgit_show(const char *id_str) {
    Snapshot *snap = NULL;

    if (id_str) {
        snap = load_snapshot_from_disk(atoi(id_str));
        if (!snap)
            return;
        printf("SNAPSHOT %d: %s\n", snap->snapshot_id, snap->message);
    } else {
        snap = calloc(1, sizeof(Snapshot));
        uint32_t head = get_current_head();

        Snapshot *prev = (head > 0) ? load_snapshot_from_disk(head) : NULL;
        snap->files = build_file_list_bfs(".", prev ? prev->files : NULL);

        printf("LIVE VIEW\n");
        if (prev)
            free_snapshot(prev);
    }

    FileEntry *c = snap->files;
    while (c) {
        printf("%s %s\n", c->is_directory ? "DIR " : "FILE", c->path);
        c = c->next;
    }

    free_snapshot(snap);
}
