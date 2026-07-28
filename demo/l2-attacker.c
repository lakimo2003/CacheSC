#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cachesc.h>

#define ATTACKER_CPU 7

#define NUMBER_OF_SETS 3

#define GO_FIFO   "/tmp/cachesc_go_fifo"
#define DONE_FIFO "/tmp/cachesc_done_fifo"

static uint32_t monitored_sets[NUMBER_OF_SETS] = {
    30,
    60,
    99
};

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

static char receive_byte(int fd)
{
    char value;
    ssize_t result;

    do {
        result = read(fd, &value, 1);
    } while (result < 0 && errno == EINTR);

    if (result != 1) {
        fprintf(stderr, "Failed to receive victim response\n");
        exit(EXIT_FAILURE);
    }

    return value;
}

/*
 * Perform one Prime+Probe round.
 *
 * command == 0:
 *     Victim wakes up but does not access its cache line.
 *
 * command == 1:
 *     Victim accesses its selected cache line.
 */
static void measure_round(
    cacheline *attacker_sets,
    int go_fd,
    int done_fd,
    char command,
    time_type *measurements)
{
    /*
     * Fill all ways of sets 30, 60 and 99.
     */
    cacheline *probe_head = prime_rev(attacker_sets);

    /*
     * Tell the victim what to do.
     */
    send_byte(go_fd, command);

    /*
     * Wait until the victim has finished.
     */
    receive_byte(done_fd);

    /*
     * Probe the same three sets.
     */
    probe(L2, probe_head);

    memset(
        measurements,
        0,
        L2_SETS * sizeof(*measurements)
    );

    get_msrmts_for_all_set(
        probe_head,
        measurements
    );
}

int main(int argc, char **argv)
{
    uint32_t samples = 10000;

    if (argc == 2) {
        samples = (uint32_t) strtoul(argv[1], NULL, 10);
    }

    if (samples == 0) {
        fprintf(stderr, "Usage: %s [number-of-samples]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * Use CPU7. CPU7 and CPU8 share the same L2.
     */
    pin_to_cpu(ATTACKER_CPU);

    fprintf(
        stderr,
        "Attacker PID %d running on CPU%d\n",
        getpid(),
        ATTACKER_CPU
    );

    cache_ctx *ctx = get_cache_ctx(L2);

    /*
     * Create eviction sets only for sets 30, 60 and 99.
     */
    cacheline *attacker_sets =
        prepare_cache_set_ds(
            ctx,
            monitored_sets,
            NUMBER_OF_SETS
        );

    /*
     * Create synchronization pipes.
     *
     * Remove leftovers from an earlier execution first.
     */
    unlink(GO_FIFO);
    unlink(DONE_FIFO);

    if (mkfifo(GO_FIFO, 0666) != 0) {
        perror("mkfifo go");
        return EXIT_FAILURE;
    }

    if (mkfifo(DONE_FIFO, 0666) != 0) {
        perror("mkfifo done");
        unlink(GO_FIFO);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Waiting for victim program...\n");

    /*
     * These calls block until the victim opens its sides.
     */
    int go_fd = open(GO_FIFO, O_WRONLY);

    if (go_fd < 0) {
        perror("open go FIFO");
        return EXIT_FAILURE;
    }

    int done_fd = open(DONE_FIFO, O_RDONLY);

    if (done_fd < 0) {
        perror("open done FIFO");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Victim connected. Starting attack.\n");

    uint64_t baseline_sums[NUMBER_OF_SETS] = {0};
    uint64_t attack_sums[NUMBER_OF_SETS] = {0};

    time_type measurements[L2_SETS];

    prepare_measurement();

    for (uint32_t sample = 0; sample < samples; sample++) {
        /*
         * Control round:
         *
         * The victim wakes and responds, but does not access
         * its selected cache line.
         */
        measure_round(
            attacker_sets,
            go_fd,
            done_fd,
            0,
            measurements
        );

        for (uint32_t i = 0; i < NUMBER_OF_SETS; i++) {
            baseline_sums[i] +=
                measurements[monitored_sets[i]];
        }

        /*
         * Attack round:
         *
         * The victim accesses its selected cache line.
         */
        measure_round(
            attacker_sets,
            go_fd,
            done_fd,
            1,
            measurements
        );

        for (uint32_t i = 0; i < NUMBER_OF_SETS; i++) {
            attack_sums[i] +=
                measurements[monitored_sets[i]];
        }
    }

    /*
     * Closing this pipe tells the victim to exit.
     */
    close(go_fd);
    close(done_fd);

    printf("\nResults over %u samples:\n\n", samples);

    double largest_difference = -1000000.0;
    uint32_t detected_set = 0;

    for (uint32_t i = 0; i < NUMBER_OF_SETS; i++) {
        double baseline =
            (double) baseline_sums[i] / samples;

        double attack =
            (double) attack_sums[i] / samples;

        double difference = attack - baseline;

        printf("Set %u\n", monitored_sets[i]);
        printf("  Baseline:   %.2f cycles\n", baseline);
        printf("  With touch: %.2f cycles\n", attack);
        printf("  Difference: %.2f cycles\n\n", difference);

        if (difference > largest_difference) {
            largest_difference = difference;
            detected_set = monitored_sets[i];
        }
    }

    printf(
        "Attacker predicts that the victim touched set %u\n",
        detected_set
    );

    release_cache_set_ds(ctx, attacker_sets);
    release_cache_ctx(ctx);

    unlink(GO_FIFO);
    unlink(DONE_FIFO);

    return EXIT_SUCCESS;
}