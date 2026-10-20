#include "line_ring.h"

#include <string.h>

bool line_ring_push(LineRing *ring, const char *line)
{
    unsigned head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail >= LINE_RING_CAPACITY || strlen(line) >= LINE_SIZE) return false;
    strcpy(ring->lines[head % LINE_RING_CAPACITY], line);
    atomic_store_explicit(&ring->head, head + 1, memory_order_release);
    return true;
}

bool line_ring_pop(LineRing *ring, char *out)
{
    unsigned tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (tail == head) return false;
    strcpy(out, ring->lines[tail % LINE_RING_CAPACITY]);
    atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
    return true;
}
