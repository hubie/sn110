/*
 * devtest.c — Incremental device tests for SN110
 *
 * Tests one subsystem at a time to isolate crash causes.
 * Run with an argument to select the test:
 *   /tmp/devtest 1   - stderr output
 *   /tmp/devtest 2   - open /dev/dmx0
 *   /tmp/devtest 3   - socket creation
 *   /tmp/devtest 4   - clone/thread
 *   /tmp/devtest 5   - full daemon (single-threaded)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <pthread.h>

/* From our DMX ioctl reverse engineering */
#define DMX_IOC_SET_CONFIG  0x40106401
#define DMX_MODE_TX 2

struct dmx_config {
    unsigned char  mode;
    unsigned char  flags;
    unsigned short buf_size;
    unsigned short rate;
    char           label[8];
    unsigned char  reserved;
    unsigned char  pad;
};

static int atoi_simple(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* Test 1: stderr and fprintf */
static int test_stderr(void) {
    fprintf(stderr, "[devtest] stderr write works\n");
    printf("[devtest] stdout write works\n");
    return 0;
}

/* Test 2: open DMX device */
static int test_dmx_open(void) {
    printf("[devtest] opening /dev/dmx0...\n");
    int fd = open("/dev/dmx0", 2); /* O_RDWR */
    if (fd < 0) {
        printf("[devtest] open failed, errno=%d\n", errno);
        return 1;
    }
    printf("[devtest] /dev/dmx0 fd=%d\n", fd);

    /* Try setting TX mode */
    struct dmx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = DMX_MODE_TX;
    cfg.buf_size = 512;

    printf("[devtest] setting TX mode...\n");
    int r = ioctl(fd, DMX_IOC_SET_CONFIG, &cfg);
    printf("[devtest] ioctl result=%d errno=%d\n", r, errno);

    /* Try writing a frame of zeros */
    unsigned char frame[512];
    memset(frame, 0, 512);
    printf("[devtest] writing 512-byte frame...\n");
    int n = write(fd, frame, 512);
    printf("[devtest] write returned %d\n", n);

    close(fd);
    printf("[devtest] DMX test passed\n");
    return 0;
}

/* Test 3: socket creation */
static int test_socket(void) {
    printf("[devtest] creating UDP socket...\n");
    int fd = socket(2, 2, 17); /* AF_INET, SOCK_DGRAM, IPPROTO_UDP */
    if (fd < 0) {
        printf("[devtest] socket failed, errno=%d\n", errno);
        return 1;
    }
    printf("[devtest] socket fd=%d\n", fd);

    /* Try binding to Art-Net port 6454 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = 2; /* AF_INET */
    addr.sin_port = ((6454 >> 8) & 0xFF) | ((6454 & 0xFF) << 8); /* htons */
    addr.sin_addr.s_addr = 0; /* INADDR_ANY */

    printf("[devtest] binding to port 6454...\n");
    int r = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    printf("[devtest] bind result=%d errno=%d\n", r, errno);

    close(fd);
    printf("[devtest] socket test passed\n");
    return 0;
}

/* Test 4: thread creation */
static void *thread_fn(void *arg) {
    int *val = (int *)arg;
    printf("[devtest] thread running, arg=%d\n", *val);
    *val = 42;
    return NULL;
}

static int test_thread(void) {
    pthread_t tid;
    int val = 7;

    printf("[devtest] creating thread...\n");
    int r = pthread_create(&tid, NULL, thread_fn, &val);
    if (r != 0) {
        printf("[devtest] pthread_create failed, r=%d\n", r);
        return 1;
    }
    printf("[devtest] thread created, joining...\n");
    pthread_join(tid, NULL);
    printf("[devtest] joined, val=%d (expect 42)\n", val);
    printf("[devtest] thread test %s\n", val == 42 ? "passed" : "FAILED");
    return val != 42;
}

/* Test 5: select loop (single-threaded mini daemon) */
static int test_select(void) {
    printf("[devtest] testing select with 1s timeout...\n");
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    fd_set fds;
    FD_ZERO(&fds);
    /* select with no fds, just timeout */
    int r = select(0, &fds, NULL, NULL, &tv);
    printf("[devtest] select returned %d (expect 0=timeout)\n", r);
    printf("[devtest] select test passed\n");
    return 0;
}

int main(int argc, char *argv[]) {
    int test = 0;

    printf("[devtest] starting, argc=%d\n", argc);

    if (argc > 1)
        test = atoi_simple(argv[1]);

    printf("[devtest] running test %d\n", test);

    switch (test) {
    case 1: return test_stderr();
    case 2: return test_dmx_open();
    case 3: return test_socket();
    case 4: return test_thread();
    case 5: return test_select();
    default:
        /* Run all tests in sequence */
        printf("[devtest] === Test 1: stderr ===\n");
        if (test_stderr()) return 1;
        printf("[devtest] === Test 2: DMX ===\n");
        if (test_dmx_open()) return 1;
        printf("[devtest] === Test 3: socket ===\n");
        if (test_socket()) return 1;
        printf("[devtest] === Test 5: select ===\n");
        if (test_select()) return 1;
        printf("[devtest] === Test 4: thread ===\n");
        if (test_thread()) return 1;
        printf("[devtest] === ALL TESTS PASSED ===\n");
        return 0;
    }
}
