/* packet.c - common telemetry packet encoder/decoder with CRC-16. */
#include "packet.h"
#include "comms.h"
#include <math.h>

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

static int16_t enc_val(float v, float scale)
{
    if (isnan(v)) return PKT_NO_VALUE;
    float s = v * scale;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32767.0f) s = -32767.0f;
    return (int16_t)lrintf(s);
}

static float dec_val(int16_t r, float scale)
{
    return (r == PKT_NO_VALUE) ? NAN : (float)r / scale;
}

uint8_t pkt_status_byte(uint8_t state, bool valid, uint8_t rd)
{
    return (uint8_t)((state & 7u) << 5 | (valid ? 1u : 0u) << 4 | (rd & 15u));
}

const char *pkt_type_name(uint8_t t)
{
    static const char *n[] = {"?", "DATA", "SUMMARY", "UPDATE", "ALERT", "FAULT"};
    return t <= PKT_FAULT ? n[t] : "?";
}

/* Encoded size of p, or -1 if the packet is not valid for its profile
 * (unknown profile/type, or a channel bit beyond the profile's channels). */
int packet_encoded_len(const packet_t *p)
{
    if (p == NULL) return -1;
    const app_profile_t *prof = profile_get(p->profile_id);
    if (!prof || p->type < PKT_DATA || p->type > PKT_FAULT) return -1;
    if (p->mask & ~((1u << prof->n_channels) - 1u)) return -1;
    size_t per_ch = (p->type == PKT_SUMMARY) ? 8 : 4;
    size_t n = PKT_HDR_LEN + PKT_CRC_LEN;
    for (int i = 0; i < prof->n_channels; i++)
        if (p->mask & (1u << i)) n += per_ch;
    return (int)n;
}

int packet_encode(const packet_t *p, uint8_t *buf, size_t cap)
{
    if (p == NULL || buf == NULL) return -1;
    int total = packet_encoded_len(p);
    if (total < 0 || (size_t)total > cap) return -1;                  /* nothing written */
    const app_profile_t *prof = profile_get(p->profile_id);
    size_t n = PKT_HDR_LEN;
    buf[0] = (uint8_t)(PKT_VERSION << 4 | (p->type & 15u));
    put16(&buf[1], p->node_id);
    buf[3] = p->profile_id;
    put16(&buf[4], p->seq);
    buf[6] = (uint8_t)p->timestamp; buf[7] = (uint8_t)(p->timestamp >> 8);
    buf[8] = (uint8_t)(p->timestamp >> 16); buf[9] = (uint8_t)(p->timestamp >> 24);
    buf[10] = (uint8_t)((p->priority & 3u) << 6 | (p->node_state & 7u) << 3 | (p->flags & 7u));
    buf[11] = p->mask & 0x0F;
    for (int i = 0; i < prof->n_channels; i++) {
        if (!(p->mask & (1u << i))) continue;
        float sc = prof->ch[i].pkt_scale;
        buf[n++] = p->ch[i].sensor_id;
        buf[n++] = p->ch[i].status;
        put16(&buf[n], (uint16_t)enc_val(p->ch[i].value, sc)); n += 2;
        if (p->type == PKT_SUMMARY) {
            put16(&buf[n], (uint16_t)enc_val(p->ch[i].vmin, sc)); n += 2;
            put16(&buf[n], (uint16_t)enc_val(p->ch[i].vmax, sc)); n += 2;
        }
    }
    put16(&buf[n], crc16_ccitt_false(buf, n));
    return total;
}

int packet_decode(const uint8_t *buf, size_t len, packet_t *p)
{
    if (buf == NULL || p == NULL) return -1;
    if (len < PKT_HDR_LEN + PKT_CRC_LEN) return -1;
    if (crc16_ccitt_false(buf, len - 2) != get16(&buf[len - 2])) return -2;
    if ((buf[0] >> 4) != PKT_VERSION) return -1;
    p->type = buf[0] & 15u;
    p->node_id = get16(&buf[1]);
    p->profile_id = buf[3];
    p->seq = get16(&buf[4]);
    p->timestamp = (uint32_t)buf[6] | (uint32_t)buf[7] << 8 | (uint32_t)buf[8] << 16 | (uint32_t)buf[9] << 24;
    p->priority = buf[10] >> 6;
    p->node_state = (buf[10] >> 3) & 7u;
    p->flags = buf[10] & 7u;
    p->mask = buf[11];                 /* bits above the profile's channels are rejected below */
    const app_profile_t *prof = profile_get(p->profile_id);
    if (!prof) return -3;
    if (p->type < PKT_DATA || p->type > PKT_FAULT) return -1;
    if (p->mask & ~((1u << prof->n_channels) - 1u)) return -1;
    size_t n = PKT_HDR_LEN;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        p->ch[i].value = p->ch[i].vmin = p->ch[i].vmax = NAN;
        p->ch[i].sensor_id = 0; p->ch[i].status = 0;
        if (!(p->mask & (1u << i)) || i >= prof->n_channels) continue;
        size_t need = (p->type == PKT_SUMMARY) ? 8 : 4;
        if (n + need + PKT_CRC_LEN > len) return -1;
        float sc = prof->ch[i].pkt_scale;
        p->ch[i].sensor_id = buf[n++];
        p->ch[i].status = buf[n++];
        p->ch[i].value = dec_val((int16_t)get16(&buf[n]), sc); n += 2;
        if (p->type == PKT_SUMMARY) {
            p->ch[i].vmin = dec_val((int16_t)get16(&buf[n]), sc); n += 2;
            p->ch[i].vmax = dec_val((int16_t)get16(&buf[n]), sc); n += 2;
        } else {
            p->ch[i].vmin = p->ch[i].vmax = p->ch[i].value;
        }
    }
    return (n + PKT_CRC_LEN == len) ? 0 : -1;
}

void packet_set_flags(uint8_t *buf, size_t len, uint8_t flags)
{
    if (buf == NULL || len < PKT_HDR_LEN + PKT_CRC_LEN) return;
    buf[10] = (uint8_t)((buf[10] & ~7u) | (flags & 7u));
    put16(&buf[len - 2], crc16_ccitt_false(buf, len - 2));
}
