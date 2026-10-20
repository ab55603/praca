#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define COUNT 4

int main(void)
{
    int fd = open("/dev/midi0", O_RDONLY);
    unsigned char buffer[COUNT];
    while (1)
    {
        ssize_t bytes_read = read(fd, buffer, COUNT);
        if (bytes_read <= 0)
            continue;
        for (ssize_t i = 0; i < bytes_read; ++i)
        {
            printf("%02x", buffer[i]);
			if ((i+1) % 4 == 0) printf(" ");
        }
        printf("\n");
        fflush(stdout);
    }
    close(fd);

    return 0;
}
