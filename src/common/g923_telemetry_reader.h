/*
 * g923_telemetry_reader.h — read the SCS telemetry shared-memory block and turn
 * it into the values the TrueForce telemetry source wants. v2.
 */
#ifndef G923_TELEMETRY_READER_H
#define G923_TELEMETRY_READER_H

#include <stdbool.h>
#include "g923_telemetry_shm.h"
#include "g923_trueforce.h"

typedef struct {
    int   fd;
    void *map;
    size_t map_len;
    g923_telemetry_shm prev;   /* previous snapshot, for derivatives */
    bool  have_prev;
} g923_telemetry_reader;

/* Open the shared-memory segment the plugin publishes. Returns false if it does
 * not exist yet (game not running / plugin not installed). */
bool g923_telemetry_reader_open(g923_telemetry_reader *r);
void g923_telemetry_reader_close(g923_telemetry_reader *r);

/* Take a consistent snapshot into `out`. Returns false if unavailable. */
bool g923_telemetry_reader_sample(g923_telemetry_reader *r, g923_telemetry_shm *out);

/* Derive a 0..1 road-roughness estimate from suspension-deflection change
 * between the last two samples. Uses reader state; call once per sample. */
float g923_telemetry_road_roughness(g923_telemetry_reader *r, const g923_telemetry_shm *cur);

/* Map a snapshot (+ derived roughness) into the TrueForce telemetry context that
 * g923_tf_source_telemetry consumes. */
void g923_telemetry_to_tf(const g923_telemetry_shm *snap, float roughness,
                          g923_tf_telemetry_ctx *tf);

#endif /* G923_TELEMETRY_READER_H */
