/* storage.c - circular record store (oldest record overwritten when full). */
#include "storage.h"
#include <string.h>

void storage_init(storage_t *s, const storage_backend_t *be, uint32_t capacity)
{
    s->be = *be; s->capacity = capacity ? capacity : 1u;
    s->head = 0; s->count = 0; s->next_seq = 1; s->overwritten = 0;
}

uint32_t storage_append(storage_t *s, log_record_t *r)
{
    r->seq = s->next_seq++;
    s->be.write(s->be.ctx, s->head, r);
    s->head = (s->head + 1) % s->capacity;
    if (s->count < s->capacity) s->count++; else s->overwritten++;
    return r->seq;
}

uint32_t storage_oldest_seq(const storage_t *s)
{
    return s->count ? s->next_seq - s->count : 0;
}

int storage_get_by_seq(const storage_t *s, uint32_t seq, log_record_t *out)
{
    uint32_t oldest = storage_oldest_seq(s);
    if (s->count == 0 || seq < oldest || seq >= s->next_seq) return -1;
    uint32_t back = s->next_seq - seq;                 /* 1 = newest */
    uint32_t slot = (s->head + s->capacity - back) % s->capacity;
    return s->be.read(s->be.ctx, slot, out);
}

uint32_t storage_read_range(const storage_t *s, uint32_t from_seq, log_record_t *out, uint32_t max)
{
    uint32_t n = 0;
    if (from_seq < storage_oldest_seq(s)) from_seq = storage_oldest_seq(s);
    for (uint32_t q = from_seq; q < s->next_seq && n < max; q++)
        if (storage_get_by_seq(s, q, &out[n]) == 0) n++;
    return n;
}

/* RAM backend: ctx is a log_record_t array of 'capacity' entries. */
static int ram_write(void *ctx, uint32_t slot, const log_record_t *r) { ((log_record_t *)ctx)[slot] = *r; return 0; }
static int ram_read(void *ctx, uint32_t slot, log_record_t *r) { *r = ((log_record_t *)ctx)[slot]; return 0; }

void storage_ram_backend(storage_backend_t *be, log_record_t *mem)
{
    be->write = ram_write; be->read = ram_read; be->ctx = mem;
}
