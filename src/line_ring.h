#ifndef LINE_RING_H
#define LINE_RING_H

#include <stdbool.h>
#include <stdatomic.h>

#define LINE_RING_CAPACITY 64u
#define LINE_SIZE 2048u

typedef struct {
    atomic_uint head;
    atomic_uint tail;
    char lines[LINE_RING_CAPACITY][LINE_SIZE];
} LineRing;

bool line_ring_push(LineRing *ring, const char *line);
bool line_ring_pop(LineRing *ring, char *out);

#endif
