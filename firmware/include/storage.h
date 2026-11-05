/*
 * storage.h - circular local record store with pluggable backend
 * (RAM array on the MCU target, file-backed "flash" in the native simulator).
 * Every sample is stored here with a sequence number, so raw readings remain
 * retrievable even when the adaptive strategy does not transmit them.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "node_types.h"

typedef struct {
    uint32_t seq, ts;
    uint8_t  node_state, mode, valid_mask, n;
    float    value[MAX_CHANNELS];   /* calibrated reading  */
    float    filt[MAX_CHANNELS];    /* filtered value      */
} log_record_t;

typedef struct {
    int (*write)(void *ctx, uint32_t slot, const log_record_t *r);
    int (*read)(void *ctx, uint32_t slot, log_record_t *r);
    void *ctx;
} storage_backend_t;

typedef struct {
    storage_backend_t be;
    uint32_t capacity, head, count, next_seq, overwritten;
} storage_t;

void     storage_init(storage_t *s, const storage_backend_t *be, uint32_t capacity);
uint32_t storage_append(storage_t *s, log_record_t *r);          /* assigns r->seq */
int      storage_get_by_seq(const storage_t *s, uint32_t seq, log_record_t *out);
uint32_t storage_oldest_seq(const storage_t *s);
uint32_t storage_read_range(const storage_t *s, uint32_t from_seq, log_record_t *out, uint32_t max);
void     storage_ram_backend(storage_backend_t *be, log_record_t *mem);

#endif
