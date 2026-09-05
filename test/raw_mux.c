/* SPDX-License-Identifier: MPL-2.0 */
/* Linux reproducible ingress-gate regression; no external STUN/TURN service. */
#include <juice/juice.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static atomic_int observed;
static unsigned char packet[56] = {
    0,1,0,36,0x21,0x12,0xa4,0x42,0,1,2,3,4,5,6,7,8,9,10,11,
    0,6,0,7,'h','o','s','t',':','p','e',0,
    0,8,0,20, /* MESSAGE-INTEGRITY is intentionally all zero */
};
static bool deny(const void *data, size_t size, const char *address, uint16_t port, void *ptr) {
    (void) ptr;
    assert(size == sizeof(packet));
    assert(memcmp(data, packet, size) == 0);
    assert(strcmp(address, "127.0.0.1") == 0);
    assert(port != 0);
    atomic_fetch_add(&observed, 1);
    return false;
}
static void send_packet(int fd, int port) {
    struct sockaddr_in dst = {.sin_family=AF_INET, .sin_port=htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    assert(sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&dst, sizeof(dst)) == sizeof(packet));
}
static juice_mux_stats_t await_packet(int port, uint64_t count) {
    juice_mux_stats_t stats;
    for (int i=0; i<200; ++i) {
        assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
        if (stats.received >= count) return stats;
        usleep(5000);
    }
    assert(!"timed out waiting for ingress");
    return stats;
}
int main(void) {
    // Legacy lookup promotes a forged-integrity packet by known ufrag. This
    // comparison reproduces the missing guarantee before installing the gate.
    juice_config_t legacy={0};
    legacy.concurrency_mode=JUICE_CONCURRENCY_MODE_MUX;
    legacy.bind_address="127.0.0.1";
    legacy.local_port_range_begin=legacy.local_port_range_end=49181;
    juice_agent_t *old=juice_create(&legacy); assert(old);
    assert(juice_set_local_ice_attributes(old,"host","012345678901234567890123456789")==0);
    assert(juice_gather_candidates(old)==0);
    int attack=socket(AF_INET,SOCK_DGRAM,0); assert(attack>=0);
    send_packet(attack,49181);
    juice_mux_stats_t before;
    for (int i=0; i<200; ++i) {
        assert(juice_mux_get_stats("127.0.0.1",49181,&before)==0);
        if(before.mapped_tuples==1) break;
        usleep(5000);
    }
    assert(before.mapped_tuples==1);
    juice_destroy(old); close(attack);
    int fd=socket(AF_INET, SOCK_DGRAM, 0); assert(fd>=0);
    const int port=49182;
    assert(juice_mux_listen_raw("127.0.0.1", port, deny, NULL)==0);
    assert(juice_mux_listen_raw("127.0.0.1", port, deny, NULL)!=0);
    // Same port on a distinct address is a distinct endpoint, never an alias.
    assert(juice_mux_listen_raw("127.0.0.2", port, deny, NULL)==0);
    assert(juice_mux_listen_raw("127.0.0.2", port, NULL, NULL)==0);
    send_packet(fd, port);
    juice_mux_stats_t s=await_packet(port, 1);
    assert(s.agents==0 && s.mapped_tuples==0 && s.rejected==1);
    juice_config_t config={0};
    config.concurrency_mode=JUICE_CONCURRENCY_MODE_MUX;
    config.bind_address="127.0.0.1";
    config.local_port_range_begin=config.local_port_range_end=port;
    juice_agent_t *a=juice_create(&config); assert(a);
    assert(juice_set_local_ice_attributes(a,"host","012345678901234567890123456789")==0);
    assert(juice_gather_candidates(a)==0);
    send_packet(fd, port);
    s=await_packet(port, 2);
    assert(s.agents==1 && s.mapped_tuples==0 && s.rejected==2);
    // Removing the listener while a peer exists must never turn admission off.
    assert(juice_mux_listen_raw("127.0.0.1",port,NULL,NULL)==0);
    send_packet(fd, port);
    s=await_packet(port, 3);
    assert(s.agents==1 && s.mapped_tuples==0 && s.rejected==3);
    assert(atomic_load(&observed)==2);
    juice_destroy(a);
    close(fd);
    // Deterministic port release after the last agent/listener.
    fd=socket(AF_INET,SOCK_DGRAM,0); assert(fd>=0);
    struct sockaddr_in bind_addr={.sin_family=AF_INET,.sin_port=htons(port)};
    inet_pton(AF_INET,"127.0.0.1",&bind_addr.sin_addr);
    assert(bind(fd,(struct sockaddr *)&bind_addr,sizeof(bind_addr))==0);
    close(fd);
    puts("raw-mux: raw bytes, zero pre-admission agents/maps, existing-peer gate, fail-closed stop, high port, cleanup PASS");
}
