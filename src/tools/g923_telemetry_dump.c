/*
 * g923_telemetry_dump — read the SCS telemetry shared memory and print it. v2.
 *
 * Use this (no wheel needed) to confirm the SCS plugin is publishing data:
 * install the plugin, launch ETS2/ATS, drive, and watch RPM/speed change here.
 *
 *   g923_telemetry_dump           print once
 *   g923_telemetry_dump --watch   refresh ~10x/s until Ctrl-C
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "g923_telemetry_reader.h"

static void print_snap(g923_telemetry_reader *r, const g923_telemetry_shm *s) {
    float rough = g923_telemetry_road_roughness(r, s);
    printf("\rconn=%u paused=%u  rpm=%6.0f/%6.0f  speed=%5.1f km/h  gear=%+d  wheels=%u  roughness=%.2f   ",
           s->connected, s->paused, s->engine_rpm, s->engine_rpm_max,
           s->speed_ms * 3.6f, s->gear, s->wheel_count, rough);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int watch = (argc > 1 && !strcmp(argv[1], "--watch"));
    g923_telemetry_reader r;
    if (!g923_telemetry_reader_open(&r)) {
        fprintf(stderr, "telemetry shared memory '%s' not found.\n", G923_TELEMETRY_SHM_NAME);
        fprintf(stderr, "Install the SCS plugin (see src/scs-plugin/README.md) and launch ETS2/ATS.\n");
        return 1;
    }
    g923_telemetry_shm s;
    do {
        if (g923_telemetry_reader_sample(&r, &s)) print_snap(&r, &s);
        else { printf("\r(no consistent sample yet)          "); fflush(stdout); }
        if (watch) usleep(100000);
    } while (watch);
    printf("\n");
    g923_telemetry_reader_close(&r);
    return 0;
}
