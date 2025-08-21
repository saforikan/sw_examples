#ifndef TAP_IF_H
#define TAP_IF_H

/**
 * Interface to the tap device configuration and status registers.
 *
 * This is a placeholder for the formal API which is currently under development.
 * The API will support:
 *      Stats API  : Read hw stats for processed packets and datagrams, including errors
 *      Filter API : Set filter configurations and read per filter stats
 *
 * This version of the application accesses the device's memory-mapped hardware registers
 * directly through hardcoded offsets, within the device's allocated memory space, by means of mmap.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>

/* Device PCIe attributes: BAR2 is user-addressable */
const uint32_t DEVICE_MEMSIZE = 4*1024*1024;	/* PCIe BAR2 size */
const uint32_t DEVICE_MEMOFFSET = 4096;	/* PCIe BAR2 page offset */
const uint32_t DGGEN_BASE_ADDR = 12288;	/* Datagram Generator (DGGEN) offset within BAR2 */

/* TAP config values */
#define NUM_TAP_PORTS 4
#define DGGEN_ALIGNMENT_BYTES 4

/* DGGEN statistics */
typedef struct dggen_stats {
	uint32_t seq_num;
	uint32_t size_settings;     /* 13 bits */
	uint32_t timer_settings;    /* 17 bits */
	uint32_t in_subframe_cnt;
	uint32_t out_subframe_cnt;
} dggen_stats_t;

typedef struct hw_port_stats {
	uint32_t bit_field;
	uint32_t total_subframes;
	uint32_t buffer_full_trunc_cnt;
	uint32_t packet_size_trunc_cnt;
	uint32_t buffer_full_drop_cnt;
	uint32_t out_packet_count;
	uint32_t trunc_cnt_usr_req;
	uint32_t phy_rxerr_cnt;
	uint32_t drop_cnt_usr_req;
} port_stats_t;

typedef struct hw_stats {
	dggen_stats_t dggen_stats;
	port_stats_t port_stats[NUM_TAP_PORTS];
} hw_stats_t;



int get_dggen_stat(hw_stats_t *hw_stats)
{
	if (hw_stats == NULL)
		return -1;

	int fd = open("/dev/uio0", O_RDWR|O_SYNC);

	if (fd <= 0) {
		perror("Cannot find Tap device");
		return fd;
	}

	uint8_t *register_map = (uint8_t *)
		mmap(NULL, DEVICE_MEMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DEVICE_MEMOFFSET);
	close(fd);

	if (register_map == (void *)-1) {
		perror("Cannot map PCIe memory");
		return -1;
	}

	hw_stats->dggen_stats = *(dggen_stats_t *)(register_map + DGGEN_BASE_ADDR);
	for (int port_num = 0; port_num < NUM_TAP_PORTS; port_num++)
		hw_stats->port_stats[port_num] = *(port_stats_t *) (register_map + 16384 + port_num*4096);

	if (munmap((void *)register_map, DEVICE_MEMSIZE) == -1)
		perror("Error unmapping device registers");
	return 0;
}

#endif
