/*
 * packet.h - common telemetry packet format (all three profiles).
 *
 *  off size field
 *   0   1   version(4b) | type(4b)
 *   1   2   node_id            (little endian)
 *   3   1   profile_id
 *   4   2   sequence number
 *   6   4   timestamp [s]
 *  10   1   priority(2b) | node_state(3b) | flags(3b)
 *  11   1   channel mask (bit i = channel i present)
 *  12   n   per present channel: sensor_id(1) status(1) value(int16)
 *           SUMMARY adds min(int16) max(int16)
 *  end  2   CRC-16/CCITT-FALSE over all previous bytes (little endian)
 *
 * channel status byte: state(3b) | valid(1b) | read_status(4b)
 */
#ifndef PACKET_H
#define PACKET_H

#include "profile.h"

#define PKT_VERSION     1
#define PKT_HDR_LEN     12
#define PKT_CRC_LEN     2
#define PKT_MAX_LEN     64
#define PKT_FLAG_STORED 0x01   /* delivered from the store-and-forward queue */
#define PKT_NO_VALUE    ((int16_t)0x8000)

typedef enum { PKT_DATA = 1, PKT_SUMMARY = 2, PKT_UPDATE = 3, PKT_ALERT = 4, PKT_FAULT = 5 } pkt_type_t;

typedef struct { uint8_t sensor_id, status; float value, vmin, vmax; } pkt_channel_t;

typedef struct {
    uint8_t  type;
    uint16_t node_id;
    uint8_t  profile_id;
    uint16_t seq;
    uint32_t timestamp;
    uint8_t  priority, node_state, flags, mask;
    pkt_channel_t ch[MAX_CHANNELS];   /* indexed by channel number */
} packet_t;

int  packet_encoded_len(const packet_t *p);                         /* bytes needed, -1 if invalid */
int  packet_encode(const packet_t *p, uint8_t *buf, size_t cap);   /* length, or -1 (buffer untouched) */
int  packet_decode(const uint8_t *buf, size_t len, packet_t *p);   /* 0 ok, -1 length, -2 CRC, -3 profile */
void packet_set_flags(uint8_t *buf, size_t len, uint8_t flags);    /* patch flags + recompute CRC */
uint8_t pkt_status_byte(uint8_t state, bool valid, uint8_t rd_status);
const char *pkt_type_name(uint8_t t);

#endif
