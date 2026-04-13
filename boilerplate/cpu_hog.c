#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned int parse_seconds(const char *arg, unsigned int fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);

    if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
        return fallback;
    return (unsigned int)value;
}

int main(int argc, char *argv[])
{
    const unsigned int duration = (argc > 1) ? parse_seconds(argv[1], 20) : 20;
    const time_t start = time(NULL);
    time_t last_report = start;
    volatile unsigned long long acc = 0;

    while ((unsigned int)(time(NULL) - start) < duration) {
        acc = acc * 1664525ULL + 1013904223ULL;

        if (time(NULL) != last_report) {
            last_report = time(NULL);
            printf("cpu_hog alive elapsed=%ld acc=%llu\n",
                   (long)(last_report - start), acc);
            fflush(stdout);
        }
    }

    printf("cpu_hog done duration=%u acc=%llu\n", duration, acc);
    return 0;
}
