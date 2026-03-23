#include <dirent.h>
#include <errno.h>

#include "mgit.h"

// --- Helper Functions ---
uint32_t get_current_head() {
    int saved_value;
    int fd = open(".mgit/HEAD", O_RDONLY);

    if (fd != -1) {
        read(fd, &saved_value, sizeof(int));
    } else {
        fprintf(stderr, "Error reading .mgit/HEAD file");
        saved_value = 0;
    }

    close(fd);
    return saved_value ? saved_value : 0;
}

void update_head(uint32_t new_id) {
    int fd = open(".mgit/HEAD", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd == -1) {
        perror("Error opening .mgit/HEAD");
    } else {
        if (write(fd, &new_id, sizeof(uint32_t)) == -1) {
            perror("Error writing to HEAD counter");
        }
    }

    close(fd);
}

// --- Blob Storage (Raw) ---
void write_blob_to_vault(const char *filepath, BlockTable *block) {
    FILE *src = fopen(filepath, "rb");
    if (!src) {
        perror("Error opening filepath");
        return;
    }

    FILE *vault = fopen(".mgit/data.bin", "ab");
    if (!vault) {
        perror("Error opening .mgit/data.bin");
        fclose(src);
        return;
    }

    block->physical_offset = ftell(vault);

    char buffer[4096];
    size_t bytes_in, total_size = 0;

    while ((bytes_in = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes_in, vault);
        total_size += bytes_in;
    }

    block->compressed_size = (uint32_t) total_size;

    fclose(src);
    fclose(vault);
}

void read_blob_from_vault(uint64_t offset, uint32_t size, int out_fd) {
    FILE *vault = fopen(".mgit/data.bin", "rb");

    if (!vault) {
        perror("Error opening .mgit/data.bin");
        return;
    }

    if (fseek(vault, offset, SEEK_SET) != 0) {
        perror("fseek failed");
        fclose(vault);
        return;
    }

    char buffer[4096];
    uint32_t rem = size;

    while (rem > 0) {
        size_t size_read = (rem < sizeof(buffer) ? rem : sizeof(buffer));
        size_t bytes_read = fread(buffer, 1, size_read, vault);

        if (size_read > bytes_read) {
            if (ferror(vault))
                perror("Error reading from data.bin into buffer");
            break;
        }

        if (write_all(out_fd, buffer, bytes_read) == -1) {
            perror("Error in call to write_all");
            break;
        }

        rem -= bytes_read;
    }

    fclose(vault);
}

// --- Snapshot Management ---
void store_snapshot_to_disk(Snapshot *snap) {
    char filename[512];
    sprintf(filename, ".mgit/snapshots/snap_%03u.bin", snap->snapshot_id);

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to create snapshot file");
        return;
    }

    fwrite(snap, sizeof(Snapshot), 1, fp);
    FileEntry *current = snap->files;

    while (current) {
        fwrite(current, sizeof(FileEntry), 1, fp);

        if (current->num_blocks > 0 && current->chunks != NULL) {
            fwrite(current->chunks, sizeof(BlockTable), current->num_blocks, fp);
        }

        current = current->next;
    }

    fclose(fp);
}

Snapshot *load_snapshot_from_disk(uint32_t id) {
    char filename[512];
    sprintf(filename, ".mgit/snapshots/snap_%03u.bin", id);

    FILE *fp = fopen(filename, "rb");
    if (!fp)
        return NULL;

    Snapshot *snap = malloc(sizeof(Snapshot));
    if (fread(snap, sizeof(Snapshot), 1, fp) != 1) {
        free(snap);
        fclose(fp);
        return NULL;
    }
    snap->files = NULL;

    FileEntry **next_ptr = &(snap->files);

    for (uint32_t i = 0; i < snap->file_count; i++) {
        FileEntry *entry = malloc(sizeof(FileEntry));
        if (fread(entry, sizeof(FileEntry), 1, fp) != 1) {
            free(entry);
            fclose(fp);
            return NULL;
        }

        if (entry->num_blocks > 0) {
            entry->chunks = malloc(sizeof(BlockTable) * entry->num_blocks);
            fread(entry->chunks, sizeof(BlockTable), entry->num_blocks, fp);
        } else {
            entry->chunks = NULL;
        }

        entry->next = NULL;
        *next_ptr = entry;
        next_ptr = &(entry->next);
    }

    fclose(fp);
    return snap;
}

void chunks_recycle(uint32_t target_id) {
    // 1. Load the oldest snapshot (target_id) and the newest snapshot (HEAD).
    Snapshot *old = load_snapshot_from_disk(target_id);
    uint32_t head_id = get_current_head();
    Snapshot *head = load_snapshot_from_disk(head_id);
    if (!old || !head)
        return;

    // 2. Iterate through the oldest snapshot's files.
    FileEntry *old_file = old->files;
    while (old_file) {
        for (int i = 0; i < old_file->num_blocks; i++) {
            uint64_t offset = old_file->chunks[i].physical_offset;
            uint32_t size = old_file->chunks[i].compressed_size;

            int used = 0;
            FileEntry *head_file = head->files;

            while (head_file) {
                for (int j = 0; j < head_file->num_blocks; j++) {
                    if (head_file->chunks[j].physical_offset == offset) {
                        used = 1;
                        break;
                    }
                }
                if (used)
                    break;
                head_file = head_file->next;
            }

            // 3. If a chunk's physical_offset is NOT being used by ANY file in the HEAD snapshot,
            //    it is "stalled". Zero out those specific bytes in `data.bin`.
            if (!used) {
                FILE *vault = fopen(".mgit/data.bin", "rb+");
                if (!vault) {
                    perror("Failed to open vault for vacuuming");
                    return;
                }

                if (fseek(vault, offset, SEEK_SET) != 0) {
                    perror("fseek to chunk failed");
                    fclose(vault);
                    return;
                }

                char *zero_buf = calloc(1, size);
                if (!zero_buf) {
                    fprintf(stderr, "Memory allocation failed for zero_buf\n");
                    fclose(vault);
                    return;
                }

                fwrite(zero_buf, 1, size, vault);

                free(zero_buf);
                fflush(vault);
                fclose(vault);
            }
        }
        old_file = old_file->next;
    }

    free_snapshot(old);
    free_snapshot(head);
}

FileEntry *find_written_inode(FileEntry *head, FileEntry *current) {
    FileEntry *scanner = head;
    while (scanner != current) {
        if (scanner->inode == current->inode && scanner->chunks[0].physical_offset != 0) {
            return scanner;
        }
        scanner = scanner->next;
    }
    return NULL;
}

void mgit_snapshot(const char *msg) {
    int cur_head = get_current_head();
    int next_id = cur_head + 1;
    Snapshot *new_s = calloc(1, sizeof(Snapshot));

    if (!new_s) {
        perror("calloc failed for Snapshot");
        return;
    }

    new_s->snapshot_id = next_id;
    if (msg) {
        strncpy(new_s->message, msg, sizeof(new_s->message) - 1);
    }

    uint32_t count = 0;

    Snapshot *prev_s = load_snapshot_from_disk(cur_head);

    FileEntry *prev_f = (prev_s != NULL) ? prev_s->files : NULL;

    FileEntry *new_d = build_file_list_bfs(".", prev_f);

    if (prev_s)
        free_snapshot(prev_s);

    // - If a file has data (chunks) but its size is 0, it needs to be written to the vault.
    // - CRITICAL: Check for Hard Links! If another file in the *current* list with the same
    //   inode was already written to the vault, copy its offset and size. DO NOT write twice!
    // - Call write_blob_to_vault() for new files.
    FileEntry *cur = new_d;
    while (cur) {
        count++;
        if (!cur->is_directory && cur->chunks != NULL) {
            if (cur->chunks[0].physical_offset == 0) {
                FileEntry *found = find_written_inode(new_d, cur);

                if (found) {
                    cur->num_blocks = found->num_blocks;
                    memcpy(cur->checksum, found->checksum, 32);

                    if (cur->chunks)
                        free(cur->chunks);

                    cur->chunks = malloc(sizeof(BlockTable) * found->num_blocks);

                    if (cur->chunks)
                        memcpy(cur->chunks, found->chunks, sizeof(BlockTable) * found->num_blocks);
                } else {
                    write_blob_to_vault(cur->path, cur->chunks);
                }
            }
        }
        cur = cur->next;
    }

    new_s->file_count = count;
    new_s->files = new_d;
    store_snapshot_to_disk(new_s);
    update_head(next_id);

    free_snapshot(new_s);

    const char *snap_dir = ".mgit/snapshots";
    DIR *dir = opendir(snap_dir);
    if (dir) {
        struct dirent *entry;
        int snap_count = 0;
        uint32_t target_id = -1;
        char target_name[512] = {0};

        while ((entry = readdir(dir)) != NULL) {
            uint32_t id;
            if (sscanf(entry->d_name, "snap_%u.bin", &id) == 1) {
                snap_count++;
                if (id < target_id || id < 0) {
                    target_id = id;
                    strncpy(target_name, entry->d_name, sizeof(target_name) - 1);
                }
            }
        }

        closedir(dir);

        if (snap_count > 5) {
            chunks_recycle(target_id);

            char full_path[4352];
            snprintf(full_path, sizeof(full_path), "%s/%s", snap_dir, target_name);
            if (remove(full_path) != 0) {
                perror("Failed to remove oldest snapshot file");
            }
        }
    }
}
