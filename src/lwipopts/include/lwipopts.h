// SPDX-License-Identifier: MIT
#pragma once

// Minimal lwIP configuration required by
// pico_cyw43_arch_lwip_threadsafe_background. See
// pico-sdk/lib/lwip/src/include/lwip/opt.h for the full list of options and
// their defaults.

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 4000

#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1

#define TCP_WND (8 * TCP_MSS)
#define TCP_MSS 1460
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1

#define LWIP_TCPIP_CORE_LOCKING_INPUT 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETCONN_SEM_PER_THREAD 0

#ifndef NDEBUG
#define LWIP_DEBUG 1
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 1
#endif

#define ETH_PAD_SIZE 0
#define ETHARP_TRUST_IP_MAC 0
