#include "mgit.h"

// Helper: Check if a path exists in the target snapshot
int path_in_snapshot(Snapshot* snap, const char* path)
{
    FileEntry* cur = snap->files;

    while (cur) {
        if (strcmp(cur->path, path) == 0) return 1;
        cur = cur->next;
    }

    return 0;
}

// Helper: Reverse the linked list
FileEntry* reverse_list(FileEntry* head)
{
    FileEntry* prev = NULL;
    FileEntry* cur = head;
    FileEntry* next = NULL;

    while (cur != NULL) {
        next = cur->next;

        cur->next = prev;

        prev = cur;
        cur = next;
    }

    return prev;
}


void mgit_restore(const char* id_str)
{
    if (!id_str)
        return;
    uint32_t id = atoi(id_str);

    // 1. Load Target Snapshot
    Snapshot* target_snap = load_snapshot_from_disk(id);
    if (!target_snap) {
        fprintf(stderr, "Error: Snapshot %d not found.\n", id);
        exit(1);
    }

    // --- PHASE 1: SANITIZATION (The Purge) ---
    // Remove files that exist currently but NOT in the target snapshot.
    FileEntry* current_files = build_file_list_bfs(".", NULL);
    FileEntry* reversed = reverse_list(current_files);

    // If a file/dir exists on disk (but is not ".") AND is not in target_snap:
    //   - Use rmdir() if it's a directory.
    //   - Use unlink() if it's a file.

    FileEntry* cur = reversed;
    while (cur) {
        if (strcmp(cur->path, ".") == 0) {
            cur = cur->next;
            continue;
        }

        if (!path_in_snapshot(target_snap, cur->path) && 
            access(cur->path, F_OK) == 0) {
            if (cur->is_directory) {
                if (rmdir(cur->path) != 0) {
                    perror("Failed to rmdir");
                }
            } else {
                if (unlink(cur->path) != 0) {
                    perror("Failed to unlink");
                }
            }
        }
        cur = cur->next;
    }



    free_file_list(reversed);

    // --- PHASE 2: RECONSTRUCTION & INTEGRITY ---
    // TODO: Iterate through target_snap->files.

    // HINT:
    // 1. If it's a directory (and not "."), recreate it using mkdir() with 0755.
    // 2. If it's a file, open it for writing ("wb").
    // 3. For each block in curr->chunks, call read_blob_from_vault() to write the data back to disk.

    // --- INTEGRITY CHECK (Corruption Detection) ---
    // TODO: After writing a file, compute its hash using your compute_hash() function.
    // Compare the newly computed hash with the curr->checksum stored in the snapshot.
    // If they do not match (memcmp), print a corruption error, unlink() the bad file,
    // and exit(1) to abort the restore.

    // Cleanup
    free_file_list(target_snap->files);
    free(target_snap);
}
