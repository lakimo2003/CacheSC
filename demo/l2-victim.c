#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cachesc.h>

#define VICTIM_CPU 8

#define GO_FIFO   "/tmp/cachesc_go_fifo"
#define DONE_FIFO "/tmp/cachesc_done_fifo"

static int valid_set(uint32_t set)
{
    return set == 30 ||
           set == 60 ||
           set == 99;
}

static void send_byte(int fd, char value)
{
    ssize_t result;

    do {
        result = write(fd, &value, 1);
    } while (result < 0 && errno == EINTR);

    if (result != 1) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

/*
 * Return 0 when the attacker closes the FIFO.
 * Return 1 when one command was received.
 */
static int receive_byte(int fd, char *value)
{
    ssize_t result;

    do {
        result = read(fd, value, 1);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        return 0;
    }

    if (result != 1) {
        perror("read");
        exit(EXIT_FAILURE);
    }

    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(
            stderr,
            "Usage: %s <set: 30, 60, or 99>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    uint32_t selected_set =
        (uint32_t) strtoul(argv[1], NULL, 10);

    if (!valid_set(selected_set)) {
        fprintf(
            stderr,
            "Set must be 30, 60, or 99\n"
        );

        return EXIT_FAILURE;
    }

    /*
     * CPU8 shares its L2 with attacker CPU7.
     */
    pin_to_cpu(VICTIM_CPU);

    fprintf(
        stderr,
        "Victim PID %d running on CPU%d\n",
        getpid(),
        VICTIM_CPU
    );

    fprintf(
        stderr,
        "Victim will access L2 set %u\n",
        selected_set
    );

    cache_ctx *ctx = get_cache_ctx(L2);

    /*
     * Allocate one independent line mapping to the selected set.
     */
    cacheline *victim_line =
        prepare_victim(ctx, selected_set);

    /*
     * Connect to the attacker.
     *
     * Start the attacker before starting this program.
     */
    int go_fd = open(GO_FIFO, O_RDONLY);

    if (go_fd < 0) {
        perror("open go FIFO");
        fprintf(stderr, "Start the attacker first.\n");
        return EXIT_FAILURE;
    }

    int done_fd = open(DONE_FIFO, O_WRONLY);

    if (done_fd < 0) {
        perror("open done FIFO");
        return EXIT_FAILURE;
    }

    char command;

    while (receive_byte(go_fd, &command)) {
        if (command == 1) {
            /*
             * Perform exactly one access to the selected set.
             */
            victim(victim_line);
        }

        /*
         * Inform the attacker that this round is finished.
         */
        send_byte(done_fd, 1);
    }

    close(go_fd);
    close(done_fd);

    release_victim(ctx, victim_line);
    release_cache_ctx(ctx);

    fprintf(stderr, "Victim finished.\n");

    return EXIT_SUCCESS;
}