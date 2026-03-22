#include "mgit.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char* argv[])
{
    // Basic routing logic is provided.
    if (argc < 2)
        return 1;

    if (strcmp(argv[1], "init") == 0) {
        mgit_init();
    } else if (strcmp(argv[1], "snapshot") == 0) {
        if (argc < 3)
            return 1;
        mgit_snapshot(argv[2]);
    } else if (strcmp(argv[1], "send") == 0) {
        mgit_send(argc > 2 ? argv[2] : NULL);
    } else if (strcmp(argv[1], "receive") == 0) {
        if (argc < 3)
            return 1;
        mgit_receive(argv[2]);
    } else if (strcmp(argv[1], "show") == 0) {
        mgit_show(argc > 2 ? argv[2] : NULL);
    } else if (strcmp(argv[1], "restore") == 0) {
        if (argc < 3)
            return 1;
        mgit_restore(argv[2]);
    }
    return 0;
}

void mgit_init()
{
    struct stat st = {0};

    if (stat(".mgit", &st) == 0) {
        return;
    }

    if (mkdir(".mgit", 0755) == -1) {
        perror("Error creating .mgit");
        return;
    }
    if (mkdir(".mgit/snapshots", 0755) == -1) {
        perror("Error creating .mgit/snapshots");
        return;
    }

    int vault_fd = open(".mgit/data.bin", O_WRONLY | O_CREAT, 0644);
    if (vault_fd == -1) {
        perror("Error creating .mgit/data.bin");
        return;
    }
    close(vault_fd);

    int head_fd = open(".mgit/HEAD", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (head_fd == -1) {
        perror("Error creating .mgit/HEAD");
        return;
    }
    
    if (write(head_fd, "0", 1) == -1) {
        perror("Error initializing HEAD counter");
    }
    close(head_fd);
}
