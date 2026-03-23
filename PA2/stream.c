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
    FileEntry *cur = snap->files;
    while (cur) {
        total_size += (sizeof(FileEntry) - sizeof(void *) * 2);
        if (cur->num_blocks > 0)
            total_size += (sizeof(BlockTable) * cur->num_blocks);
        cur = cur->next;
    }

    void *buf = malloc(total_size);
    void *ptr = buf;

    memcpy(ptr, &snap->snapshot_id, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(ptr, &snap->file_count, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(ptr, snap->message, 256);
    ptr += 256;

    cur = snap->files;
    while (cur) {
        size_t fixed_size = sizeof(FileEntry) - sizeof(void *) * 2;
        memcpy(ptr, cur, fixed_size);
        ptr += fixed_size;
        if (cur->num_blocks > 0) {
            size_t blocks_size = sizeof(BlockTable) * cur->num_blocks;
            memcpy(ptr, cur->chunks, blocks_size);
            ptr += blocks_size;
        }
        cur = cur->next;
    }

    *out_len = total_size;
    return buf;
}

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

void mgit_send(const char *id_str) {
    // 1. Handshake Phase
    uint32_t magic = htonl(MAGIC_NUMBER);
    write_all(STDOUT_FILENO, &magic, 4);


    uint32_t id = (uint32_t) atoi(id_str);
    Snapshot *snap = load_snapshot_from_disk(id);
    if (!snap) exit(1);

    // 2. Manifest Phase
    size_t manifest_len;
    void *manifest_buf = serialize_snapshot(snap, &manifest_len);

    // 3. Payload Phase
    uint32_t net_len = htonl((uint32_t) manifest_len);
    write_all(STDOUT_FILENO, &net_len, 4);
    write_all(STDOUT_FILENO, manifest_buf, manifest_len);

    FILE *vault = fopen(".mgit/data.bin", "rb");
    if (!vault)
        exit(1);

    FileEntry *cur = snap->files;
    while (cur) {
        if (!cur->is_directory && cur->num_blocks > 0) {
            for (int i = 0; i < cur->num_blocks; i++) {
                fseek(vault, cur->chunks[i].physical_offset, SEEK_SET);

                char *temp_buf = malloc(cur->chunks[i].compressed_size);

                if (fread(temp_buf, 1, cur->chunks[i].compressed_size, vault) ==
                    cur->chunks[i].compressed_size) {
                    write_all(STDOUT_FILENO, temp_buf, cur->chunks[i].compressed_size);
                }
                free(temp_buf);
            }
        }
        cur = cur->next;
    }

    fclose(vault);
    free(manifest_buf);
    free_snapshot(snap);
}

void mgit_receive(const char *dest_path) {
    // 1. Setup
    mkdir(dest_path, 0755);
    chdir(dest_path);
    mgit_init();

    uint32_t magic;
    if (read_all(STDIN_FILENO, &magic, 4) != 4)
        exit(1);
    if (ntohl(magic) != MAGIC_NUMBER) {
        fprintf(stderr, "Error: Invalid protocol\n");
        exit(1);
    }

    // 2. Handshake Phase

    uint32_t net_len;
    if (read_all(STDIN_FILENO, &net_len, 4) != 4)
        exit(1);

    size_t manifest_len = ntohl(net_len);

    // 3. Manifest Reconstruction
    void *buf = malloc(manifest_len);

    if (read_all(STDIN_FILENO, buf, manifest_len) != manifest_len) {
        free(buf);
        exit(1);
    }

    Snapshot *snap = deserialize_snapshot(buf, manifest_len);
    free(buf);

    // 4. Processing Chunks (The Streaming OS Challenge)
    FILE *vault = fopen(".mgit/data.bin", "ab");
    if (!vault)
        exit(1);

    FileEntry *cur = snap->files;
    while (cur) {
        if (cur->is_directory) {
            mkdir(cur->path, 0755);
        } else if (cur->num_blocks > 0) {
            for (int i = 0; i < cur->num_blocks; i++) {
                cur->chunks[i].physical_offset = ftell(vault);

                char *chunk_data = malloc(cur->chunks[i].compressed_size);

                if (read_all(STDIN_FILENO, chunk_data, cur->chunks[i].compressed_size) ==
                    cur->chunks[i].compressed_size) {
                    fwrite(chunk_data, 1, cur->chunks[i].compressed_size, vault);
                } else {
                    free(chunk_data);
                    exit(1);
                }
                free(chunk_data);
            }
        }
        cur = cur->next;
    }

    // 5. Cleanup
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

    // Logic for loading a specific snapshot vs. a live view
    if (id_str) {
        /* 1. Convert id_str to an integer and load the snapshot */
        snap = load_snapshot_from_disk(atoi(id_str));
        if (!snap)
            return;
        printf("SNAPSHOT %d: %s\n", snap->snapshot_id, snap->message);
    } else {
        /* 2. Initialize a new snapshot structure for the live view */
        snap = calloc(1, sizeof(Snapshot));
        uint32_t head = get_current_head();

        Snapshot *prev = (head > 0) ? load_snapshot_from_disk(head) : NULL;
        /* 3. Populate the file list using a BFS traversal starting at "." */
        snap->files = build_file_list_bfs(".", prev ? prev->files : NULL);

        printf("LIVE VIEW\n");
        if (prev)
            free_snapshot(prev);
    }

    /* 4. Traverse and print the file list */
    FileEntry *c = snap->files;
    while (c) {
        printf("%s %s\n", c->is_directory ? "DIR " : "FILE", c->path);
        /* 5. Move to the next entry */
        c = c->next;
    }
    /* 6. Clean up resources to prevent memory leaks */
    free_snapshot(snap);
}
