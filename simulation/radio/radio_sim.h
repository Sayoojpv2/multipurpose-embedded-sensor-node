/* radio_sim.h - simulated radio channel and gateway (receiver endpoint). */
#ifndef RADIO_SIM_H
#define RADIO_SIM_H

#include "packet.h"

typedef struct { float p_loss, p_ack_loss, p_corrupt; uint32_t seed; } radio_sim_cfg_t;
typedef struct { uint32_t tx, lost_outage, lost_channel, corrupted, ack_lost; } radio_sim_stats_t;

void radio_sim_init(const radio_sim_cfg_t *c);
const radio_sim_stats_t *radio_sim_stats(void);

typedef void (*gw_log_cb)(void *ctx, uint32_t t_rx, const packet_t *p, uint8_t len);
typedef struct { uint32_t rx_ok, crc_fail, decode_fail, duplicates, bytes; } gw_stats_t;

void gateway_init(gw_log_cb cb, void *ctx);
int  gateway_receive(const uint8_t *buf, uint8_t len, uint32_t t);   /* 1 = valid -> ACK */
const gw_stats_t *gateway_stats(void);

#endif
