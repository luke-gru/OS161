#ifndef _NET_INET_H_
#define _NET_INET_H_

#include <types.h>

// NOTE: functions provided by bswap.c object file
uint32_t htonl(uint32_t x); // host to network long
uint16_t htons(uint16_t x); // host to network short
uint32_t ntohl(uint32_t x); // network to host long
uint16_t ntohs(uint16_t x); // network to host short

#endif
