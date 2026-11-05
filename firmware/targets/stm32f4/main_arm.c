/*
 * main_arm.c - STM32F407 (Cortex-M4) image for the Renode stm32f4_discovery
 * platform.
 *
 * What is emulated: the Cortex-M4 CPU executing the unchanged firmware core,
 * flash/RAM, and the USART2 peripheral used as console.
 * What is NOT emulated: sensors and the radio. Sensor values come from the
 * virtual sensor backend fed by an embedded trace (trace_data.h); the radio is
 * the same simulated channel/gateway model as on the host. This is clearly a
 * software-in-the-loop run on an emulated MCU, not hardware validation.
 */
#include "node.h"
#include "hal.h"
#include "../../../simulation/sensors/virtual_sensors.h"
#include "../../../simulation/radio/radio_sim.h"
#include "trace_data.h"

#define USART2_BASE 0x40004400u
#define USART_SR  (*(volatile uint32_t *)(USART2_BASE + 0x00u))
#define USART_DR  (*(volatile uint32_t *)(USART2_BASE + 0x04u))
#define USART_BRR (*(volatile uint32_t *)(USART2_BASE + 0x08u))
#define USART_CR1 (*(volatile uint32_t *)(USART2_BASE + 0x0Cu))
#define RCC_APB1ENR (*(volatile uint32_t *)0x40023840u)

void target_console_write(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        while (!(USART_SR & (1u << 7))) { }   /* TXE */
        USART_DR = d[i];
    }
}

static void puts_(const char *s) { size_t n = 0; while (s[n]) n++; hal_uart_write(HAL_UART_CONSOLE, (const uint8_t *)s, n); }

static void putu(uint32_t v)
{
    char b[11]; int i = 10; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10u); v /= 10u; } while (v);
    puts_(&b[i]);
}

static void putf(float v, int dec)          /* fixed-point print without printf */
{
    if (v != v) { puts_("nan"); return; }
    if (v < 0) { puts_("-"); v = -v; }
    uint32_t scale = 1; for (int i = 0; i < dec; i++) scale *= 10u;
    uint32_t x = (uint32_t)(v * (float)scale + 0.5f);
    putu(x / scale);
    if (dec) {
        puts_(".");
        uint32_t f = x % scale;
        for (uint32_t s = scale / 10u; s > 1u && f < s; s /= 10u) puts_("0");
        putu(f);
    }
}

static env_trace_t g_trace = {
    TRACE_ROWS, TRACE_DT_S,
    {TRACE_V0, TRACE_V1, TRACE_V2, TRACE_V3},
    {TRACE_F0, TRACE_F1, TRACE_F2, TRACE_F3},
    TRACE_LINK
};

static uint32_t g_gw_alerts;
static void gw_cb(void *ctx, uint32_t t, const packet_t *p, uint8_t len)
{
    (void)ctx;
    if (p->type != PKT_ALERT && p->type != PKT_FAULT) return;   /* keep the log short */
    g_gw_alerts++;
    puts_("  GW t="); putu(t); puts_(" seq="); putu(p->seq); puts_(" "); puts_(pkt_type_name(p->type));
    puts_(" state="); putu(p->node_state); puts_(" len="); putu(len);
    puts_(" ch0="); putf(p->ch[0].value, 2); puts_("\r\n");
}

static log_record_t g_mem[512];
static node_t g_node;

static void run(uint8_t strategy, const char *name)
{
    storage_t st; storage_backend_t be;
    storage_ram_backend(&be, g_mem);
    storage_init(&st, &be, 512);
    env_set_trace(&g_trace);
    vdev_load_board(PROFILE_WATER);
    radio_sim_cfg_t rc = {0.02f, 0.02f, 0.005f, 1u};
    radio_sim_init(&rc);
    g_gw_alerts = 0;
    gateway_init(gw_cb, 0);
    node_config_t c; node_default_config(&c);
    c.profile_id = PROFILE_WATER; c.strategy = strategy; c.seed = 1;
    if (node_init(&g_node, &c, &st)) { puts_("node_init failed\r\n"); return; }
    puts_("strategy "); puts_(name); puts_("\r\n");
    uint32_t dur = env_duration_s(), t = 0;
    while (t < dur) { hal_sim_set_time(t); t = node_step(&g_node, t); }
    hal_sim_set_time(dur);
    link_service(&g_node.link, dur);
    node_finish(&g_node, dur);
    const link_stats_t *l = &g_node.link.st;
    puts_("  samples="); putu(g_node.n_samples);
    puts_(" packets="); putu(g_node.n_packets);
    puts_(" tx_attempts="); putu(l->attempts);
    puts_(" delivered="); putu(l->delivered);
    puts_(" airtime_ms="); putf(l->airtime_ms, 1);
    puts_(" energy_mJ="); putf((float)(energy_total_mas(&g_node.e) * c.energy.v_supply), 1);
    puts_(" records="); putu(st.next_seq - 1);
    puts_(" crc_fail_at_gw="); putu(gateway_stats()->crc_fail);
    puts_("\r\n");
}

int main(void)
{
    RCC_APB1ENR |= (1u << 17);           /* USART2 clock enable (real silicon) */
    USART_BRR = 0x0683u;                 /* 9600 Bd @ 16 MHz HSI (ignored by Renode) */
    USART_CR1 = (1u << 13) | (1u << 3);  /* UE | TE */
    puts_("\r\nMultipurpose sensor node fw " FW_VERSION_STR " on STM32F407 (Renode)\r\n");
    puts_("profile " TRACE_PROFILE_NAME ", embedded trace rows="); putu(TRACE_ROWS);
    puts_(" dt="); putu(TRACE_DT_S); puts_(" s\r\n");
    puts_("crc16(\"123456789\")=0x");
    uint16_t crc = crc16_ccitt_false((const uint8_t *)"123456789", 9);
    const char *hex = "0123456789ABCDEF";
    char h[5] = {hex[crc >> 12], hex[(crc >> 8) & 15], hex[(crc >> 4) & 15], hex[crc & 15], 0};
    puts_(h); puts_("\r\n");
    run(STRAT_FIXED, "fixed");
    run(STRAT_ADAPTIVE, "adaptive");
    puts_("DONE\r\n");
    for (;;) { __asm volatile ("wfi"); }
}
