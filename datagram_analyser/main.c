#include <signal.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_pmd_qdma.h>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_branch_prediction.h>
#include <rte_timer.h>

#include "main.h"
#include "datagram_header.h"
#include "tap_if.h"
#include "crc32.h"

/* DPDK config values */
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS           8191
#define MBUF_CACHE_SIZE     RTE_MEMPOOL_CACHE_MAX_SIZE
#define MBUF_DATAROOM_SIZE  (16*1024+RTE_PKTMBUF_HEADROOM)


const uint32_t BURST_SIZE = 16;
const uint32_t BUFFER_SIZE = 16*1024;

struct dev_stat {
	uint64_t datagram_cnt;
	uint32_t last_sequence;
	uint32_t err_magic_val;
} __rte_aligned(RTE_CACHE_LINE_SIZE);

struct port_stat {
	uint64_t subframe_cnt;
	uint64_t err_len_mismatch;
	uint64_t err_seq_num_mismatch;
} __rte_aligned(RTE_CACHE_LINE_SIZE);



struct lcore_conf per_lcore_config[RTE_MAX_LCORE];
struct dev_stat dggen_stat;
struct port_stat per_port_stat[NUM_TAP_PORTS]; /* stat for each capture port */

hw_stats_t hw_stats_before, hw_stats_after;

bool terminate;

/**
 * Catch SIGINT to print stats on exit
 */
static void print_stats(void)
{
	printf("\033c"); /* POSIX clear screen, not portable and terminal dependent */
	printf("DGGEN Frames: %"PRIu64"\n", dggen_stat.datagram_cnt);

	if (dggen_stat.err_magic_val != 0)
		printf("Ignored %"PRIu32" datagrams due to incorrect magic values\n", dggen_stat.err_magic_val);

	for (int port_id = 0; port_id < NUM_TAP_PORTS; port_id++) {
		printf("Port %u  Recieved subframes  : %"PRIu64"\n",
				port_id,
				per_port_stat[port_id].subframe_cnt);
		printf("        Truncated subframes : %"PRIu64"\n",
				per_port_stat[port_id].err_len_mismatch);
		printf("        Dropped subframes   : %"PRIu64"\n",
				per_port_stat[port_id].err_seq_num_mismatch);
		printf("\n");
	}
	printf("\n");

	printf("HW stats for this run\n");
	get_dggen_stat(&hw_stats_after);
	printf("  Frame cnt : %u\n", hw_stats_after.dggen_stats.seq_num - hw_stats_before.dggen_stats.seq_num);
	printf("  In cnt    : %u\n", hw_stats_after.dggen_stats.in_subframe_cnt - hw_stats_before.dggen_stats.in_subframe_cnt);
	printf("  Out cnt   : %u\n", hw_stats_after.dggen_stats.out_subframe_cnt - hw_stats_before.dggen_stats.out_subframe_cnt);
	printf("  Size      : %u\n\n", hw_stats_after.dggen_stats.size_settings - hw_stats_before.dggen_stats.size_settings);

	for (int port_num = 0; port_num < NUM_TAP_PORTS; port_num++) {
		printf("  Port %d  ", port_num);
		printf("Total subframes       : %u\n", hw_stats_after.port_stats[port_num].total_subframes - hw_stats_before.port_stats[port_num].total_subframes);
		printf("\t  Out packet count      : %u\n", hw_stats_after.port_stats[port_num].out_packet_count - hw_stats_before.port_stats[port_num].out_packet_count);
		printf("\t  Truncated full buffer : %u\n", hw_stats_after.port_stats[port_num].buffer_full_trunc_cnt - hw_stats_before.port_stats[port_num].buffer_full_trunc_cnt);
		printf("\t  Dropped full buffer   : %u\n", hw_stats_after.port_stats[port_num].buffer_full_drop_cnt - hw_stats_before.port_stats[port_num].buffer_full_drop_cnt);
		printf("\t  Dropped user req      : %u\n", hw_stats_after.port_stats[port_num].drop_cnt_usr_req - hw_stats_before.port_stats[port_num].drop_cnt_usr_req);
		printf("\t  Truncated user req    : %u\n", hw_stats_after.port_stats[port_num].trunc_cnt_usr_req - hw_stats_before.port_stats[port_num].trunc_cnt_usr_req);
		printf("\n");
	}
}

/**
 * Text UI - periodically print stats
 */
static void tui_main(__rte_unused struct rte_timer *timer, __rte_unused void *arg)
{
	fflush(stdout);
	print_stats();
}

/**
 * Main processing function
 * A lcore is assigned with a port to process the incoming dggen traffic
 */
static int lcore_main(__rte_unused void *arg)
{
	unsigned int lcore_id;

	lcore_id = rte_lcore_id();
	if (!per_lcore_config[lcore_id].active)
		return 0;

	RTE_LOG(INFO, USER1, "Core %u processing port %u\n", lcore_id, per_lcore_config[lcore_id].dev_port_id);
	unsigned int dev_port_id = per_lcore_config[lcore_id].dev_port_id;
	struct rte_mbuf *bufs[BURST_SIZE];
	uint32_t expected_seq_val = 0;
	uint64_t dggen_datagram_cnt = 0;
	bool init_run = true;

	while (!terminate) {
		const uint16_t nb_rx = rte_eth_rx_burst(dev_port_id, 0, bufs, BURST_SIZE);

		if (likely(nb_rx == 0))
			continue;

		dggen_stat.datagram_cnt += nb_rx;

		for (int mbuf_idx = 0; mbuf_idx < nb_rx; mbuf_idx++) {
			uint8_t *packet_ptr = rte_pktmbuf_mtod(bufs[mbuf_idx], uint8_t*);
			uint16_t packet_len = rte_pktmbuf_pkt_len(bufs[mbuf_idx]);
			uint16_t packet_idx = 0;

			expected_seq_val++;

			datagram_header_t dg_header = parse_dg_hdr(packet_ptr);

			if (unlikely(init_run)) {
				expected_seq_val = dg_header.sequence_number;
				printf("Init sequence count: %u\n", expected_seq_val);
				init_run = false;
			}

			if (dg_header.magic != 0x0002) {
				for (int i = 0; i < packet_len; i++)
					printf("%02X", packet_ptr[i]);
				printf("\n");
				printf("Failed magic value\n");
				dggen_stat.err_magic_val++;
				continue;
			}
			packet_idx += 8; /* skip dggen header */

			uint32_t subframe_cnt = 0;

			while (packet_idx < packet_len) {
				uint8_t *sf_ptr = packet_ptr + packet_idx;

				subframe_header_t sf_header = parse_subframe_hdr(sf_ptr);

				packet_idx += 16; /* skip subframe header */
				sf_ptr = packet_ptr + packet_idx;

				uint8_t port_id = sf_header.port_num;

				uint16_t aligned_sf_len;

				if (sf_header.captured_len % DGGEN_ALIGNMENT_BYTES != 0)
					aligned_sf_len = sf_header.captured_len + DGGEN_ALIGNMENT_BYTES - (sf_header.captured_len % DGGEN_ALIGNMENT_BYTES);
				else
					aligned_sf_len = sf_header.captured_len;

				if (sf_header.captured_len != sf_header.original_len)
					per_port_stat[port_id].err_len_mismatch++;

				if (aligned_sf_len < 1)
					continue;

				/* Verify CRC32 */
				uint32_t crc = crc32(sf_ptr+8, sf_header.captured_len-8);

				if (crc != 0x2144DF1C) {
					printf("Incorrect FCS value: seq %u, subframe %u, datagram size %u\n", expected_seq_val, subframe_cnt, packet_len);
					for (int i = 0; i < sf_header.captured_len - 8; i++)
						printf("%02X", (sf_ptr+8)[i]);
					printf("\n");
				}

				packet_idx += aligned_sf_len;
				per_port_stat[port_id].subframe_cnt++;
				subframe_cnt++;
			}

			if (unlikely(expected_seq_val != dg_header.sequence_number)) {
				printf("WARNING: Expected sequence: %u, captured sequence: %u\n", dg_header.sequence_number, expected_seq_val);
				expected_seq_val = dg_header.sequence_number;
			}

			if (unlikely(subframe_cnt != dg_header.frames))
				printf("WARNING: Expected datagrams in DGGEN: %u, captured frames: %u\n", dg_header.frames, subframe_cnt);

			rte_pktmbuf_free(bufs[mbuf_idx]);
		}
		dggen_stat.last_sequence = expected_seq_val;
	}

	return 0;
}

/*
 * Port initialization, derived from DPDK skeleton example
 */
static int port_init(uint16_t port_id)
{
	struct rte_eth_conf port_conf;
	struct rte_eth_dev_info dev_info;

	const uint16_t rx_rings = 1, tx_rings = 0;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;

	printf("Initialising port %d\n", port_id);
	if (!rte_eth_dev_is_valid_port(port_id))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	int retval = rte_eth_dev_info_get(port_id, &dev_info);

	if (retval != 0) {
		RTE_LOG(WARNING, EAL, "Error during getting device (port %u) info: %s\n", port_id, strerror(-retval));
		return retval;
	}

	retval = rte_eth_dev_configure(port_id, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	rte_pmd_qdma_get_device(port_id);

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	char mbuf_name[20];

	snprintf(mbuf_name, 20, "%s%d", "MBUF_POOL", port_id);
	struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(mbuf_name, NUM_MBUFS, MBUF_CACHE_SIZE, 0, MBUF_DATAROOM_SIZE, rte_socket_id());

	for (int q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port_id, q, nb_rxd, rte_socket_id(), NULL, mbuf_pool);
		if (retval != 0)
			return retval;
	}

	retval = rte_eth_dev_start(port_id);
	if (retval < 0) {
		RTE_LOG(ERR, EAL, "Error during getting device (port %u) info: %s\n", port_id, strerror(-retval));
		return retval;
	}

	return 0;
}

static void assign_ports_to_lcore(void)
{
	unsigned int lcore_id = rte_get_next_lcore(-1, 1, 0);
	unsigned int dev_id;

	RTE_ETH_FOREACH_DEV(dev_id) {
		if (port_init(dev_id) != 0)
			continue;

		per_lcore_config[lcore_id].dev_port_id = dev_id;
		per_lcore_config[lcore_id].active = true;
		lcore_id = rte_get_next_lcore(lcore_id, 1, 0);
	}
}

static void sigterm_handler(int sigint)
{
	print_stats();
	terminate = 1;
}

int main(int argc, char **argv)
{
	int ret;
	unsigned int lcore_id;

	signal(SIGINT, sigterm_handler);

	/* Initialization of Environment Abstraction Layer (EAL). */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");

	argc -= ret;
	argv += ret;

	ret = get_dggen_stat(&hw_stats_before);

	uint16_t nb_ports = rte_eth_dev_count_avail();

	if (nb_ports < 1)
		rte_exit(EXIT_FAILURE, "No readable ethernet ports detected, please make sure to bind the DPDK compatible driver to the capturing Ethernet port\n");

	assign_ports_to_lcore();

	/* Launches the function on each lcore. */
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
	    rte_eal_remote_launch(lcore_main, NULL, lcore_id);
	}

	struct rte_timer stat_timer;

	rte_timer_subsystem_init();
	rte_timer_init(&stat_timer);
	rte_timer_reset(&stat_timer, rte_get_timer_hz(), PERIODICAL, rte_lcore_id(), tui_main, NULL);

	uint64_t prev_stat_tsc = 0, current_tsc = 0;

	while (!terminate) {
		current_tsc = rte_rdtsc();
		if (current_tsc-prev_stat_tsc > rte_get_timer_hz()) {
			rte_timer_manage();
			prev_stat_tsc = current_tsc;
		}
	}
	rte_eal_mp_wait_lcore();

	/* clean up the EAL */
	rte_eal_cleanup();
	return 0;
}
