#include "mgit.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

// Helper to calculate SHA256 using system utility
void compute_hash(const char* path, uint8_t* output)
{
    int pipes[2];
    if (pipe(pipes) == -1) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();

    //Child process
    if (pid == 0) { 
        close(pipes[0]);   
        dup2(pipes[1], STDOUT_FILENO);
        int n_fd = open("/dev/null", O_WRONLY);

        if (n_fd != -1) {
            dup2(n_fd, STDERR_FILENO);
            close(n_fd);
        } else {
            perror("Error opening /dev/null/");
            return;
        }

        close(pipes[1]);
        execlp("sha256sum", "sha256sum", path, (char *)NULL);

    } else {
        //Parent process
        close(pipes[1]);

        char hex_hash[65];
        ssize_t size_read = read(pipes[0], hex_hash, 64);
        hex_hash[64] = '\0';

        close(pipes[0]);
        wait(NULL);

        if (size_read != 64) {
            fprintf(stderr, "Incorrect number of bytes read from child");
            return;
        }
        
        for (int i = 0; i < 32; i++) {
            sscanf(&hex_hash[i * 2], "%02hhx", &output[i]);
        }

    }

}

// Check if file matches previous snapshot (Quick Check)
FileEntry* find_in_prev(FileEntry* prev, const char* path)
{
    while (prev) {
        if (strcmp(prev->path, path) == 0)
            return prev;
        prev = prev->next;
    }
    return NULL;
}

// HELPER: Check if an inode already exists in the current snapshot's list
FileEntry* find_in_current_by_inode(FileEntry* head, ino_t inode)
{
    while (head) {
        if (!head->is_directory && head->inode == inode)
            return head;
        head = head->next;
    }
    return NULL;
}

FileEntry* build_file_list_bfs(const char* root, FileEntry* prev_snap_files)
{
    FileEntry *head = NULL, *tail = NULL;

    // TODO: 1. Initialize the Root directory "." and add it to your BFS queue/list.
    FileEntry* root_n = calloc(1, sizeof(FileEntry));
    strcpy(root_n->path, root);
    root_n->is_directory = 1;

    head = root_n;
    tail = root_n;

    FileEntry* cur = head;

    for (FileEntry *cur = head; cur != NULL; cur = cur->next) {
        if (cur->is_directory) {
            DIR* dir = opendir(cur->path);
            if (!dir) {
                fprintf(stderr, "Error: unable to open directory");
                continue;
            }
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || 
                    strcmp(entry->d_name, "..") == 0 || 
                    strcmp(entry->d_name, ".mgit") == 0) 
                        continue;
                char full_path[4096];
                snprintf(full_path, sizeof(full_path), "%s/%s", 
                     cur->path, entry->d_name);


                struct stat st;
                if (stat(full_path, &st) == -1) {
                    fprintf(stderr, "Error: unable to get stats for file path");
                    continue;
                }
                FileEntry *new_n = calloc(1, sizeof(FileEntry));
                strncpy(new_n->path, full_path, sizeof(new_n->path) - 1);
                new_n->size = st.st_size;
                new_n->mtime = st.st_mtime;
                new_n->is_directory = S_ISDIR(st.st_mode);
                new_n->inode = st.st_ino;

                //Check if file existed previously in current state or if found already
                int changed = 0;

                if (!new_n->is_directory) {
                    FileEntry* dup_n = find_in_current_by_inode(cur, new_n->inode);
                    if (dup_n) {
                        memcpy(new_n->checksum, dup_n->checksum, 32);
                        new_n->num_blocks = dup_n->num_blocks;
                        new_n->chunks = malloc(sizeof(BlockTable) * new_n->num_blocks);
                        memcpy(new_n->chunks, dup_n->chunks, sizeof(BlockTable) * new_n->num_blocks);
                    
                        changed = 1;
                    }

                    if (!changed && prev_snap_files) {
                        dup_n = find_in_prev(prev_snap_files, new_n->path);
                        if (dup_n && dup_n->size == new_n->size 
                            && dup_n->mtime == new_n->mtime) {
                                memcpy(new_n->checksum, dup_n->checksum, 32);
                                new_n->num_blocks = dup_n->num_blocks;
                                new_n->chunks = malloc(sizeof(BlockTable));

                                //Need to deep copy to avoid issues if prev snapshot deleted
                                if (!new_n->chunks) {
                                    perror("Malloc failed for new node chunks");
                                }

                                if (dup_n->chunks) {
                                    memcpy(new_n->chunks, dup_n->chunks, sizeof(BlockTable));
                                }

                                changed = 1;
                            }
                    }
                }

                //Rehash if not
                if (!new_n->is_directory && !changed) {
                    compute_hash(new_n->path, new_n->checksum);

                    new_n->num_blocks = 1;
                    new_n->chunks = calloc(1, sizeof(BlockTable));
                }

                tail->next = new_n;
                tail = new_n;
            }
            closedir(dir);
        }
    }

    return head;
}

void free_file_list(FileEntry* head)
{
    FileEntry* cur = head;

    while (cur) {
        FileEntry* next = cur->next;

        if (cur->chunks) {
            free(cur->chunks);
        }

        free(cur);
        cur = next;
    }
}

void free_snapshot(Snapshot* snap) {
    if (!snap) return;

    FileEntry* cur = snap->files;
    free_file_list(cur);

    free(snap);
}
