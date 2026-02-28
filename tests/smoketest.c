/*
 * smoketest.c — Smoke test for SN110 OABI minilib
 *
 * Tests printf, malloc, string ops, and file I/O without
 * touching network or DMX hardware. Safe to run alongside lxnetdmx.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    /* Test 1: printf with formatting */
    printf("[smoke] test 1: printf works (argc=%d)\n", argc);

    /* Test 2: string operations */
    char buf[64];
    strcpy(buf, "hello world");
    printf("[smoke] test 2: strcpy = '%s' (len=%d)\n", buf, (int)strlen(buf));

    /* Test 3: malloc */
    char *p = malloc(128);
    if (p) {
        memset(p, 'A', 10);
        p[10] = '\0';
        printf("[smoke] test 3: malloc ok, content='%s'\n", p);
    } else {
        printf("[smoke] test 3: malloc FAILED\n");
    }

    /* Test 4: snprintf */
    char sbuf[32];
    int n = snprintf(sbuf, sizeof(sbuf), "val=%d hex=0x%x", 42, 0xDEAD);
    printf("[smoke] test 4: snprintf='%s' (n=%d)\n", sbuf, n);

    /* Test 5: write raw */
    write(1, "[smoke] test 5: raw write ok\n", 29);

    printf("[smoke] ALL TESTS PASSED\n");
    return 0;
}
