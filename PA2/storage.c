#include "mgit.h"
#include <errno.h>

// --- Helper Functions ---
uint32_t get_current_head()
{

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

void update_head(uint32_t new_id)
{
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
void write_blob_to_vault(const char* filepath, BlockTable* block)
{
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

    block->compressed_size = (uint32_t)total_size;

    fclose(src);
    fclose(vault);
}

void read_blob_from_vault(uint64_t offset, uint32_t size, int out_fd)
{
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

    while(rem > 0) {
        size_t size_read = (rem < sizeof(buffer) ? rem : sizeof(buffer));
        size_t bytes_read = fread(buffer, 1, size_read, vault);

        if (size_read > bytes_read) {
            if (ferror(vault)) perror("Error reading from data.bin into buffer");
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
void store_snapshot_to_disk(Snapshot* snap)
{
    char filename[512];
    sprintf(filename, ".mgit/snapshots/snap_%03u.bin", snap->snapshot_id);

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to create snapshot file");
        return;
    }

    fwrite(snap, sizeof(Snapshot), 1, fp);
    FileEntry* current = snap->files;

    while (current) {
        fwrite(current, sizeof(FileEntry), 1, fp);

        if (current->num_blocks > 0 && current->chunks != NULL) {
            fwrite(current->chunks, sizeof(BlockTable), current->num_blocks, fp);
        }

        current = current->next;
    }

    fclose(fp);
}

Snapshot* load_snapshot_from_disk(uint32_t id)
{
    char filename[512];
    sprintf(filename, ".mgit/snapshots/snap_%03u.bin", id);

    FILE* fp = fopen(filename, "rb");
    if (!fp) return NULL;

    Snapshot* snap = malloc(sizeof(Snapshot));
    if (fread(snap, sizeof(Snapshot), 1, fp) != 1) {
        free(snap);
        fclose(fp);
        return NULL;
    }
    snap->files = NULL;

    FileEntry** next_ptr = &(snap->files);

    for (uint32_t i = 0; i < snap->file_count; i++) {
        FileEntry* entry = malloc(sizeof(FileEntry));
        if (fread(entry, sizeof(Snapshot), 1, fp) != 1) {
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


void chunks_recycle(uint32_t target_id)
{
    // 1. Load the oldest snapshot (target_id) and the newest snapshot (HEAD).
    Snapshot* old = load_snapshot_from_disk(target_id);
    uint32_t head_id = get_current_head();
    Snapshot* head = load_snapshot_from_disk(head_id);
    if (!old || !head) return;

    // 2. Iterate through the oldest snapshot's files.
    FileEntry* old_file = old->files;
    while (old_file) {
        for (int i = 0; i < old_file->num_blocks; i++) {
            uint64_t offset = old_file->chunks[i].physical_offset;
            uint32_t size = old_file->chunks[i].compressed_size;

            int used = 0;
            FileEntry* head_file = head->files;

            while (head_file) {
                for (int j = 0; j < head_file->num_blocks; j++) {
                    if (head_file->chunks[j].physical_offset == offset) {
                        used = 1;
                        break;
                    }
                }
                if (used) break;
                head_file = head_file->next;
            }

                // 3. If a chunk's physical_offset is NOT being used by ANY file in the HEAD snapshot,
                //    it is "stalled". Zero out those specific bytes in `data.bin`.
                if (!used) {
                FILE* vault = fopen(".mgit/data.bin", "rb+");
                if (!vault) {
                    perror("Failed to open vault for vacuuming");
                    return;
                }

                if (fseek(vault, offset, SEEK_SET) != 0) {
                    perror("fseek to chunk failed");
                    fclose(vault);
                    return;
                }
                
                char* zero_buf = calloc(1, size);
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


void mgit_snapshot(const char* msg)
{
    // TODO: 1. Get current HEAD ID and calculate next_id. Load previous files for crawling.
    // TODO: 2. Call build_file_list_bfs() to get the new directory state.

    // TODO: 3. Iterate through the new file list.
    // - If a file has data (chunks) but its size is 0, it needs to be written to the vault.
    // - CRITICAL: Check for Hard Links! If another file in the *current* list with the same
    //   inode was already written to the vault, copy its offset and size. DO NOT write twice!
    // - Call write_blob_to_vault() for new files.

    // TODO: 4. Call store_snapshot_to_disk() and update_head().
    // TODO: 5. Free memory.
    // TODO: 6. Enforce MAX_SNAPSHOT_HISTORY (5). If exceeded, call chunks_recycle()
    //          and delete the oldest manifest file using remove().
}
