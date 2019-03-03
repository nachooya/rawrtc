#pragma once
#include <rawrtc.h>
#include "ice_server.h"

#define RAWRTC_MAX_DNS_SERVERS 10

struct rawrtc_config {
    uint32_t pacing_interval;
    bool ipv4_enable;
    bool ipv6_enable;
    bool udp_enable;
    bool tcp_enable;
    enum rawrtc_certificate_sign_algorithm sign_algorithm;
    enum rawrtc_ice_server_transport ice_server_normal_transport;
    enum rawrtc_ice_server_transport ice_server_secure_transport;
    uint32_t stun_keepalive_interval;
    struct stun_conf stun_config;
    struct sa dns_servers[RAWRTC_MAX_DNS_SERVERS];
    uint32_t n_dns_servers;
};

extern struct rawrtc_config rawrtc_default_config;
