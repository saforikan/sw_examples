#ifndef DATAGRAM_ANALYSER
#define DATAGRAM_ANALYSER

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <rte_eal.h>

struct lcore_conf {
	bool active;
	unsigned int dev_port_id;
} __rte_cache_aligned;

#endif
