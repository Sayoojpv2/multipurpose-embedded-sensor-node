/*
 * comms.h - CRC routines, LoRa-style airtime model, radio driver interface
 * and the link layer (ACK, bounded retries, outage backoff, store-and-forward).
 *
 * NOTE: this is NOT a LoRaWAN stack. Airtime follows the Semtech LoRa
 * time-on-air formula; LoRaWAN MAC framing is represented only by a fixed
 * overhead byte count. ACKs are modelled as confirmed-uplink style downlinks.
 */
#ifndef COMMS_H
#define COMMS_H

#include "node_types.h"
#include "packet.h"

/* ---- CRC ---- */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len); /* poly 0x1021 init 0xFFFF */
uint8_t  crc8_sensirion(const uint8_t *data, size_t len);    /* poly 0x31 init 0xFF     */

/* ---- LoRa airtime ---- */
typedef struct {
    uint8_t  sf;          /* spreading factor 7..12            */
    uint32_t bw_hz;       /* 125000                            */
    uint8_t  cr;          /* coding rate 1..4 => 4/5..4/8      */
    uint8_t  preamble;    /* 8 symbols                         */
    uint8_t  mac_overhead;/* LoRaWAN-style MHDR+FHDR+FPort+MIC = 13 */
} lora_cfg_t;

float lora_airtime_ms(const lora_cfg_t *c, uint16_t phy_len, bool crc_on);

/* ---- radio driver (implemented by simulation/radio/radio_sim.c) ---- */
/* One transmission attempt on the shared millisecond timeline. */
int radio_transmit(const uint8_t *buf, uint8_t len, uint64_t tx_start_ms, float airtime_ms);

/* ---- link layer ---- */
#define TXQ_CAP 128

typedef struct {
    uint8_t  buf[PKT_MAX_LEN];
    uint8_t  len, prio, type;
    uint16_t seq;
    uint32_t created_t;
    bool     waited_outage;   /* was queued while link was down */
} txq_item_t;

typedef struct {
    uint8_t  max_retries;     /* retries after the first attempt   */
    uint32_t backoff_init_s, backoff_max_s;
    uint32_t backoff_urgent_max_s; /* cap while a CRITICAL packet is queued */
    uint8_t  max_per_service; /* backlog packets sent per wake-up  */
    uint32_t stored_age_s;    /* older packets get PKT_FLAG_STORED */
    float    rx_window_ms;    /* listen time when no ACK arrives   */
    uint8_t  ack_len;         /* downlink ACK PHY payload bytes    */
} link_cfg_t;

typedef struct {
    uint32_t submitted, delivered, dropped_queue, attempts, retries, failed_sends;
    uint32_t bytes_air, bytes_delivered, recovered_after_outage, outages, max_queue;
    float    airtime_ms, rx_ms;
} link_stats_t;

typedef void (*link_attempt_cb)(void *ctx, uint32_t t, const txq_item_t *it, uint8_t attempt, float airtime_ms, bool acked);

typedef struct {
    txq_item_t q[TXQ_CAP];
    uint16_t   count;
    bool       up;
    uint32_t   next_probe_t, backoff_s;
    uint64_t   next_tx_ms;          /* serialized TX/ACK timeline */
    link_cfg_t cfg;
    lora_cfg_t lora;
    link_stats_t st;
    uint32_t   rng;
    link_attempt_cb on_attempt;
    void      *cb_ctx;
} link_t;

void link_default_cfg(link_cfg_t *c);
void lora_default_cfg(lora_cfg_t *c);
void link_init(link_t *l, const link_cfg_t *c, const lora_cfg_t *lc, uint32_t seed);
int  link_submit(link_t *l, const uint8_t *buf, uint8_t len, uint8_t prio, uint16_t seq, uint8_t type, uint32_t now);
void link_service(link_t *l, uint32_t now);
uint32_t xorshift32(uint32_t *s);

#endif
