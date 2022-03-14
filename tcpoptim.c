/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright(c) 2022 P Quentin Armitage <quentin@armitage.org.uk>
 */

//#define DEBUG
#define DEBUG_LOG_ACTIONS
#define DEBUG_GARBAGE


#ifdef PQA
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <ev.h>
#include <threads.h>
#ifdef DEBUG
#include <net/if.h>
#endif

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_telemetry.h>
#include <rte_ether.h>
#ifdef DEBUG
#include <rte_ip.h>
#endif
#include <rte_timer.h>
#include <rte_malloc.h>


#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "tcp_process.h"
#include "tfo_worker.h"
#include "util.h"

#ifndef PQA
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#else
#define RX_RING_SIZE 128
#define TX_RING_SIZE 128
#endif

#ifndef PQA
#define NUM_MBUFS 8191
#else
#define NUM_MBUFS 2047
#endif
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

/* Number of physical interfaces we can handle */
#define MAX_IF	2

#define PROG_NAME "tcpoptim"

/* locals */
static uint16_t port_id[MAX_IF];
static int sigint;
static int sigterm;
static struct ev_loop *loop;
static struct ev_signal ev_sigterm;
static struct ev_signal ev_sigint;
static unsigned slowpath_time;

static uint16_t vlan_idx;
// Redefine this to be struct { uint16_t pub_vlan, uint16_t priv_vlan };
static uint16_t vlan_id[MAX_IF * 2];

static struct rte_mempool *ack_pool[RTE_MAX_NUMA_NODES];

static thread_local uint16_t priv_vlan;
static thread_local struct rte_timer garbage_timer;
static thread_local uint16_t gport_id;
static thread_local uint16_t gqueue_idx;
#ifdef DEBUG_GARBAGE
static thread_local struct timespec last_ts;
#endif

/* Doesn't need to be thread_local if our impure D-space is on the right node */
static thread_local uint64_t in_priv_mask;

static unsigned queues_per_port;

static void
do_shutdown(void)
{
	int retval = 0;
	int nb_ports = rte_eth_dev_count_avail();

	for (int i = 0; i < nb_ports; i++) {
		retval |= rte_eth_promiscuous_disable(port_id[i]);
printf("Done disable\n");
		retval |= rte_eth_dev_stop(port_id[i]);
printf("Done stop\n");
		retval |= rte_eth_dev_close(port_id[i]);
printf("Done close\n");
	}

	if (!retval)
		printf("Ports shutdown successfully\n");
	else
		printf("Port shutdown failed, retval = %d\n", retval);

	rte_eal_cleanup();
}

static int
shutdown_cmd(const char *cmd, const char *params, struct rte_tel_data *info)
{
printf("Shutdown called for pid %d tid %d\n", getpid(), gettid());
	do_shutdown();

	return 0;
}

static void
sigint_hdl(struct ev_loop *loop, struct ev_signal *w, int revents)
{
	if (++sigint > 2) {
		fprintf(stderr, "ctrl-C pressed too much, dying hard\n");
		exit(1);
	}

	if (sigint == 1) {
printf("Shutdown INT called for pid %d tid %d\n", getpid(), gettid());
		do_shutdown();
		fprintf(stderr, "shutting down\n");
		ev_break(loop, EVBREAK_ONE);
	}
}

static void
sigterm_hdl(struct ev_loop *loop, struct ev_signal *w, int revents)
{
	if (++sigterm > 2) {
		fprintf(stderr, "too many sigterm received, dying in great woe\n");
		exit(1);
	}

	if (sigterm == 1) {
printf("Shutdown TERM called for pid %d tid %d\n", getpid(), gettid());
do_shutdown();
		fprintf(stderr, "shutting down\n");
		ev_break(loop, EVBREAK_ONE);
	}
}



/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool, int ring_count)
{
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = ring_count, tx_rings = ring_count;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up x RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up x TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

static void
set_dir(struct rte_mbuf **bufs, uint16_t nb_rx, uint16_t port_id)
{
	uint16_t i;
	struct rte_mbuf *m;
	struct rte_ether_hdr *eh;
	struct rte_vlan_hdr *vh;
	uint16_t vlan_tag;
//	uint16_t priv_vlan = vlan_id[port_id * 2 + 1];

	for (i = 0; i < nb_rx; i++) {
// Can we do bufs++; ?
		m = bufs[i];
		eh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
//printf("m->vlan_tci %u, eh->ether_type %x, m->ol_flags 0x%x\n", m->vlan_tci, rte_be_to_cpu_16(eh->ether_type), m->ol_flags);

		if (eh->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN)) {
			vh = (struct rte_vlan_hdr *)(eh + 1);

			vlan_tag = rte_be_to_cpu_16(vh->vlan_tci);
		} else
			vlan_tag = 0;

		if (priv_vlan == vlan_tag)
			m->ol_flags |= in_priv_mask;
	}
}

static inline void
update_vlan_ids(struct tfo_tx_bufs *tx_bufs, uint16_t port_id)
{
	uint16_t i;
	struct rte_mbuf *m;
	struct rte_ether_hdr *eh;
	struct rte_vlan_hdr *vh;
	uint16_t vlan_tag;
	uint16_t vlan_new;
	struct rte_ether_addr sav_src_addr;
	uint16_t o = 0;
	uint16_t vlan0, vlan1;
	uint16_t nb_tx = tx_bufs->nb_tx;
#ifdef DEBUG
	struct rte_ipv4_hdr *iph = NULL;
	struct rte_ipv6_hdr *ip6h = NULL;
#endif

	vlan0 = vlan_id[port_id * 2];
	vlan1 = vlan_id[port_id * 2 + 1];
#ifdef DEBUG
	printf("update_vlan_ids %u <=> %u\n", vlan0, vlan1);
#endif

	for (i = 0; i < nb_tx; i++) {
		m = tx_bufs->m[i];
		eh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
//printf("m->vlan_tci %u, eh->ether_type %x, m->ol_flags 0x%x\n", m->vlan_tci, rte_be_to_cpu_16(eh->ether_type), m->ol_flags);

		/* This swaps Vlan 1 and Vlan 2 */
		if (eh->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN)) {
			vh = (struct rte_vlan_hdr *)(eh + 1);

			vlan_tag = rte_be_to_cpu_16(vh->vlan_tci);
		} else
			vlan_tag = 0;

#ifdef VLAN_SWAP
		if (vlan_tag == vlan0)
			vlan_new = vlan1;
		else if (vlan_tag == vlan1)
			vlan_new = vlan0;
		else {
			printf("Packet on unused vlan %u\n", vlan_tag);
			vlan_new = 4096;
		}
#else
		vlan_new = m->vlan_tci;
#endif

#ifdef DEBUG
		if (vlan_tag) {
			if (vh->eth_proto == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
				iph = (struct rte_ipv4_hdr *)(vh + 1);
			else if (vh->eth_proto == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
				ip6h = (struct rte_ipv6_hdr *)(vh + 1);
#ifdef DEBUG1
			else
				printf("vh->eth_proto = 0x%x\n", rte_be_to_cpu_16(vh->eth_proto));
#endif
		} else {
			if (eh->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
				iph = (struct rte_ipv4_hdr *)(eh + 1);
			else if (eh->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
				ip6h = (struct rte_ipv6_hdr *)(eh + 1);
#ifdef DEBUG1
			else
				printf("eh->eth_type = 0x%x\n", rte_be_to_cpu_16(eh->ether_type));
#endif
		}
		if (iph) {
			char addr[2][INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &iph->src_addr, addr[0], INET_ADDRSTRLEN);
			inet_ntop(AF_INET, &iph->dst_addr, addr[1], INET_ADDRSTRLEN);

			printf("%s -> %s\n", addr[0], addr[1]);
		}
		else if (ip6h) {
			char addr[2][INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &ip6h->src_addr, addr[0], INET6_ADDRSTRLEN);
			inet_ntop(AF_INET6, &ip6h->dst_addr, addr[1], INET6_ADDRSTRLEN);

			printf("%s -> %s\n", addr[0], addr[1]);
		}
#ifdef DEBUG1
		else
			continue;
#endif
#endif

		if (vlan_new != 4096) {
			if (vlan_new && vlan_tag) {
				vh->vlan_tci = rte_cpu_to_be_16(vlan_new);
			} else if (!vlan_new && !vlan_tag) {
				/* Do nothing */
			} else if (!vlan_new) {
				/* remove vlan encapsulation */
				eh->ether_type = vh->eth_proto;		// We could avoid this, and copy sizeof - 2
//printf("Removing vlan encap memmove(%p, %p, %u\n", rte_pktmbuf_mtod_offset(m, uint8_t *, sizeof (struct rte_vlan_hdr)), eh, sizeof (struct rte_ether_hdr));
//fflush(stdout);
				memmove(rte_pktmbuf_adj(m, sizeof (struct rte_vlan_hdr)),
					eh, sizeof (struct rte_ether_hdr));
				eh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
			} else {
				/* add vlan encapsulation */
				void *p = rte_pktmbuf_prepend(m, sizeof (struct rte_vlan_hdr));
				if (unlikely(p == NULL)) {
					rte_pktmbuf_free(m);
					continue;
				}

				/* move ethernet header at the start */
				memmove(p, eh, sizeof (struct rte_ether_hdr));		// we could do sizeof - 2
				eh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

				vh = (struct rte_vlan_hdr *)(eh + 1);
				vh->vlan_tci = rte_cpu_to_be_16(vlan_new);
				vh->eth_proto = eh->ether_type;

				eh->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
			}

			m->vlan_tci = vlan_new;

#ifdef DEBUG
			printf("Moving packet from vlan %u to %u\n", vlan_tag, vlan_new);
#endif
		}

		/* Swap MAC addresses */
//		eh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
//		net = fn_network_out(to_priv);
//		ad = fn_app_device(net->port);
		rte_ether_addr_copy(&eh->src_addr, &sav_src_addr);
		rte_ether_addr_copy(&eh->dst_addr, &eh->src_addr);
		rte_ether_addr_copy(&sav_src_addr, &eh->dst_addr);

		if (unlikely(i != o))
			tx_bufs->m[o] = tx_bufs->m[i];
		o++;
	}

	tx_bufs->nb_tx = o;
}


static inline void
fwd_packet(uint16_t port_id, uint16_t queue_idx)
{
	/* Get burst of RX packets, from first port of pair. */
	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t nb_tx;
#ifdef DEBUG
	struct rte_eth_dev_info dev_info;
	char ifname[IF_NAMESIZE];
#endif
	struct timespec ts;
	uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_idx,
						bufs, BURST_SIZE);
	struct tfo_tx_bufs tx_bufs = { .nb_tx = 0, .nb_inc = nb_rx };

	if (unlikely(nb_rx == 0))
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
#ifdef DEBUG
	char timestamp[24];
	char *p = timestamp;
	time_t t;
	struct tm tm;

	t = ts.tv_sec;
	localtime_r(&t, &tm);
	p += strftime(p, sizeof(timestamp), "%T", &tm);
	p += snprintf(p, timestamp + sizeof(timestamp) - p, ".%9.9ld", ts.tv_nsec);

	rte_eth_dev_info_get(port_id, &dev_info);
	printf("%s (%d): %s %s(%d) ", timestamp, gettid(), dev_info.driver_name, if_indextoname(dev_info.if_index, ifname), dev_info.if_index);
#endif

#ifdef DEBUG_LOG_ACTIONS
	printf("\nfwd %d packet(s) from port %d to port %d, lcore %d, queue_idx: %u\n",
	       nb_rx, port_id, port_id, rte_lcore_id(), queue_idx);
#endif

	if (nb_rx)
		nb_rx = monitor_pkts(bufs, nb_rx);
	if (nb_rx) {
		set_dir(bufs, nb_rx, port_id);
		tcp_worker_mbuf_burst(bufs, nb_rx, &ts, &tx_bufs);
	}

	if (tx_bufs.nb_tx)
		update_vlan_ids(&tx_bufs, port_id);

#ifdef DEBUG_LOG_ACTIONS
	printf("No to tx %u\n", tx_bufs.nb_tx);
#endif

	if (tx_bufs.nb_tx) {
		/* send burst of TX packets, to second port of pair. */
		nb_tx = rte_eth_tx_burst(port_id, queue_idx, tx_bufs.m, tx_bufs.nb_tx);

		/* free any unsent packets. */
// We should really only mark packets as sent here, not when they are added to tx_bufs
		if (unlikely(nb_tx < tx_bufs.nb_tx)) {
#ifdef DEBUG_LOG_ACTIONS
			printf("tx_burst %u packets sent %u packets\n", tx_bufs.nb_tx, nb_tx);
#endif
			for (uint16_t buf = nb_tx; buf < tx_bufs.nb_tx; buf++) {
#ifdef DEBUG_LOG_ACTIONS
				printf("\tm 0x%x not sent\n", tx_bufs.m[buf]);
#endif
				rte_pktmbuf_free(tx_bufs.m[buf]);
			}
		}
	}

	if (tx_bufs.m)
		rte_free(tx_bufs.m);

	printf("\n");
}


static void
garbage_cb(__rte_unused struct rte_timer *time, __rte_unused void *arg)
{
	struct timespec ts;
	struct tfo_tx_bufs tx_bufs = { .m = NULL, .nb_inc = 1024 };
	uint16_t nb_tx;

	/* time is approx hz * seconds since boot */

	clock_gettime(CLOCK_REALTIME, &ts);
#ifdef DEBUG_GARBAGE
	if (last_ts.tv_sec != ts.tv_sec)
		printf("Garbage sec %ld.%9.9d\n", ts.tv_sec, ts.tv_nsec);
#endif

	tfo_garbage_collect(ts.tv_sec & 0xffff, &tx_bufs);

	if (tx_bufs.nb_tx) {
		update_vlan_ids(&tx_bufs, gport_id);

#ifdef DEBUG_GARBAGE
		printf("No to tx on port %u queue %u: %u\n", gport_id, gqueue_idx, tx_bufs.nb_tx);
#endif
	}

	if (tx_bufs.nb_tx) {
		/* send burst of TX packets, to second port of pair. */
		nb_tx = rte_eth_tx_burst(gport_id, gqueue_idx, tx_bufs.m, tx_bufs.nb_tx);

		/* free any unsent packets. */
		if (unlikely(nb_tx < tx_bufs.nb_tx)) {
#ifdef DEBUG_GARBAGE
			printf("tx_burst %u packets sent %u packets\n", tx_bufs.nb_tx, nb_tx);
#endif

			for (uint16_t buf = nb_tx; buf < tx_bufs.nb_tx; buf++) {
#ifdef DEBUG_GARBAGE
				printf("\tm 0x%x not sent\n", tx_bufs.m[buf]);
#endif

				rte_pktmbuf_free(tx_bufs.m[buf]);
			}
		}
	}

	if (tx_bufs.m)
		rte_free(tx_bufs.m);

#ifdef DEBUG_GARBAGE
	last_ts = ts;
	fflush(stdout);
#endif
}

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static int
lcore_main(void *arg)
{
	uint16_t port = rte_lcore_id() - 1;
	uint16_t queue_idx = rte_lcore_index(port + 1) - 1;
	int ret;
	struct tfo_worker_params params;

	gport_id = port;
	gqueue_idx = queue_idx;

// This is silly re rte_lcore_index()
	printf("Core %u queue_idx %d forwarding packets. [Ctrl+C to quit]\n",
	       port + 1, queue_idx);

#ifdef DEBUG
	printf("tid %d\n", gettid());
#endif

	rte_timer_init(&garbage_timer);
	if (rte_timer_reset(&garbage_timer, rte_get_timer_hz() * slowpath_time / 1000, PERIODICAL, port + 1, garbage_cb, NULL))
		fprintf(stderr, "Failed to set garbage collection timer");

	params.params = NULL;
	params.public_vlan_tci = vlan_id[port * 2];
	params.private_vlan_tci = vlan_id[port * 2 + 1];
	params.ack_pool = ack_pool[rte_socket_id()];

	in_priv_mask = tcp_worker_init(&params);
	priv_vlan = vlan_id[port * 2 + 1];

	while (1) {
		fwd_packet(port, 0);
		fwd_packet(port, 1);

#ifdef PQA
		usleep(1000);
		rte_timer_manage();
#endif
	}

	return 0;
}

static void
print_help(const char *progname)
{
	printf("%s:\n", progname);
	printf("\t-H\t\tprint this\n");
	printf("\t-q vl[,vl]\tVlan id(s)\n");
	printf("\t-u users\tMax users\n");
	printf("\t-e flows\tMax flows\n");
	printf("\t-f flows\tMax optimised flows\n");
	printf("\t-p bufp\t\tMax buffered packets\n");
	printf("\t-s ms\t\tGarbage collection interval in ms\n");
	printf("\t-x hash\t\tUser hash size\n");
	printf("\t-X hash\t\tFlow hash size\n");
	printf("\t-t timeouts\tport:syn,est,fin TCP timeouts (port 0 = defaults)\n");
}

static int
get_val(const char *optarg)
{
	char *endptr;
	long val;

	val = strtol(optarg, &endptr, 10);
	if (*endptr || val < 0 || val > INT_MAX)
		return -1;

	return val;
}

static int
set_timeout(const char *optarg, struct tcp_config *c)
{
	char *endptr;
	long val;
	uint16_t port, to_syn, to_est, to_fin;
	struct tcp_timeouts *new_to;

	val = strtol(optarg, &endptr, 10);
	if (*endptr != ':' || val < 0 || val > UINT16_MAX)
		return -1;
	port = val;

	val = strtol(endptr + 1, &endptr, 10);
	if (*endptr != ',' || val <= 0 || val > 3600)
		return -1;
	to_syn = val;

	val = strtol(endptr + 1, &endptr, 10);
	if (*endptr != ',' || val <= 0 || val > 3600)
		return -1;
	to_est = val;

	val = strtol(endptr + 1, &endptr, 10);
	if (*endptr || val < 0 || val > UINT16_MAX)
		return -1;
	to_fin = val;

	if (port > c->max_port_to) {
		new_to = realloc(c->tcp_to, (port + 1) * sizeof(*c->tcp_to));
		if (!new_to) {
			fprintf(stderr, "Unable to allocate timeouts for port %u\n", port);
			return -2;
		}

		c->tcp_to = new_to;
		memset(&c->tcp_to[c->max_port_to + 1], 0, (port - c->max_port_to) * sizeof(*c->tcp_to));
		c->max_port_to = port;
	}

	c->tcp_to[port].to_syn = to_syn;
	c->tcp_to[port].to_est = to_est;
	c->tcp_to[port].to_fin = to_fin;

	return 0;
}

static void
set_default_timeouts(struct tcp_config *c)
{
	int i;

	for (i = 1; i < c->max_port_to; i++) {
		if (!c->tcp_to[i].to_syn) {
			c->tcp_to[i].to_syn = c->tcp_to[0].to_syn;
			c->tcp_to[i].to_est = c->tcp_to[0].to_est;
			c->tcp_to[i].to_fin = c->tcp_to[0].to_fin;
		}
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool[RTE_MAX_NUMA_NODES];
	unsigned nb_ports, queue_count;
	const char *progname = argv[0];
	char opt;
	long vlan0, vlan1;
	int val;
	char *endptr;
	struct tcp_config c = { .u_n = 100, .ef_n = 10000, .f_n = 10000 };
	char packet_pool_name[] = "packet_pool_XXX";
	unsigned next_port_id;
	unsigned i;
	uint16_t node_ports[RTE_MAX_NUMA_NODES] = { 0 };
	uint16_t socket;


	/* Initialize the Environment Abstraction Layer (EAL). */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	rte_timer_subsystem_init();

	c.tcp_to = malloc(sizeof(*c.tcp_to));
	c.max_port_to = 0;
	c.tcp_to[0].to_syn = 120;
	c.tcp_to[0].to_est = 600;
	c.tcp_to[0].to_fin = 60;
	c.slowpath_time = 2;		/* Garbage collection every 2 ms */

	while ((opt = getopt(argc, argv, ":Hq:u:e:f:p:s:x:X:t:")) != -1) {
		switch(opt) {
		case 'H':
			print_help(progname);
			exit(0);
			break;
		case 'q':
			if (vlan_idx >= sizeof(vlan_id) / sizeof(vlan_id[0])) {
				fprintf(stderr, "Too many vlan ids specified\n");
				break;
			}
			vlan0 = strtol(optarg, &endptr, 10);
			if ((*endptr && endptr[0] != ',') ||
			    vlan0 < 0 || vlan0 > 4094) {
				fprintf(stderr, "Vlan '%s' invalid\n", optarg);
				break;
			}

			if (*endptr) {
				vlan1 = strtol(endptr + 1, &endptr, 10);
				if ((*endptr && endptr[0] != ',') ||
				    vlan1 < 0 || vlan1 > 4094) {
					fprintf(stderr, "Vlan '%s' invalid\n", optarg);
					break;
				}
			} else
				vlan1 = 0;
			vlan_id[vlan_idx++] = (uint16_t)vlan0;
			vlan_id[vlan_idx++] = (uint16_t)vlan1;

			break;
		case 'u':
			val = get_val(optarg);
			if (val == -1)
				fprintf(stderr, "Invalid max users %s\n", optarg);
			else
				c.f_n = val;
			break;
		case 'e':
			val = get_val(optarg);
			if (val == -1)
				fprintf(stderr, "Invalid max flows %s\n", optarg);
			else
				c.ef_n = val;
			break;
		case 'f':
			val = get_val(optarg);
			if (val == -1)
				fprintf(stderr, "Invalid max optimized flows %s\n", optarg);
			else
				c.f_n = val;
			break;
		case 'p':
			val = get_val(optarg);
			if (val == -1)
				fprintf(stderr, "Invalid max buffered packets %s\n", optarg);
			else
				c.p_n = val;
			break;
		case 's':
			val = get_val(optarg);
			if (val == -1 || val > 100)
				fprintf(stderr, "Invalid garbage collection interval %s\n", optarg);
			else
				c.slowpath_time = val;
			break;
		case 'x':
			val = get_val(optarg);
			if (val == -1)
				fprintf(stderr, "Invalid user hash size %s\n", optarg);
			else
				c.hu_n = val;
			break;
		case 'X':
			val = get_val(optarg);
			if (val == -1)
				fprintf(stderr, "Invalid flow hash size %s\n", optarg);
			else
				c.hef_n = val;
			break;
		case 't':
			if (set_timeout(optarg, &c) == -1)
				fprintf(stderr, "Invalid TCP timeout %s\n", optarg);
			break;
		case ':':
			fprintf(stderr, "Option '%c' is missing an argument\n", optopt);
			break;
		case '?':
			fprintf(stderr, "Unknown option '%c'\n", optopt);
			break;
		}
	}

	/* check that there is at least 1 port available */
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 1)
		rte_exit(EXIT_FAILURE, "Error: should have at least 1 port\n");
	else if (nb_ports > MAX_IF) {
		printf("Warning: only the first %d ports will be used\n", MAX_IF);
		nb_ports = MAX_IF;
	}

	/* Set defaults */
	slowpath_time = c.slowpath_time;
	if (!c.p_n)
		c.p_n = c.f_n * 10;
	if (!c.hu_n)
		c.hu_n = c.u_n;
	if (!c.hef_n)
		c.hef_n = c.ef_n;
	if (c.f_n > c.ef_n) {
		fprintf(stderr, "Reducing number of optimized flows to %u (number of eflows)\n", c.ef_n);
		c.f_n = c.ef_n;
	}

	set_default_timeouts(&c);

	/* Share between workers */
	c.u_n = (c.u_n + nb_ports - 1) / nb_ports;
	c.ef_n = (c.ef_n + nb_ports - 1) / nb_ports;
	c.f_n = (c.f_n + nb_ports - 1) / nb_ports;
	c.p_n = (c.p_n + nb_ports - 1) / nb_ports;

#ifdef DEBUG
	printf("vlans");
	for (int i = 0; i < nb_ports * 2; i++)
		printf(" %u", vlan_id[i]);
	printf("\n");
#endif

	/* Creates mempools to hold the mbufs. */
	for (i = 0, next_port_id = 0; i < nb_ports; i++) {
		port_id[i] = rte_eth_find_next_owned_by(next_port_id, RTE_ETH_DEV_NO_OWNER);
		node_ports[rte_eth_dev_socket_id(port_id[i])]++;
		next_port_id = port_id[i] + 1;
	}

	/* We want 2 mempools per NUMA socket with a port on it - see rte_lcore.h */
	for (i = 0; i < RTE_MAX_NUMA_NODES; i++) {
		if (node_ports[i]) {
			snprintf(packet_pool_name, sizeof(packet_pool_name), "packet_pool_%d", i);
			mbuf_pool[i] = rte_pktmbuf_pool_create(packet_pool_name, (NUM_MBUFS + 1) * node_ports[i] - 1,
				MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, i);
			if (mbuf_pool[i] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

			/* For this to work, need to increase hugh_pages to 7 */
			snprintf(packet_pool_name, sizeof(packet_pool_name), "ack_pool_%d", i);
			ack_pool[i] = rte_pktmbuf_pool_create(packet_pool_name, ((NUM_MBUFS + 1) * node_ports[i] - 1 ) * 2 / 3,
				MBUF_CACHE_SIZE, 0, tfo_max_ack_pkt_size(), i);
			if (ack_pool[i] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create ack mbuf pool\n");
		}
	}

	/* Each thread should have its own tx and rx queue. We want to share
	 * threads (lcores) across NICs. This means that a thread reads its
	 * own rx queue and writes to its own tx queue.
	 * Each thread should run on same NUMA core as NIC it is handling. */
	/* open one rx/tx queue per lcore. */
	queue_count = rte_lcore_count() - 1;
	if (queue_count < 1)
		rte_exit(EXIT_FAILURE, "Error: should have at least 1 worker (check -l option)\n");

	/* initialize our ports. */
	for (i = 0; i < nb_ports; i++) {
		socket = rte_eth_dev_socket_id(port_id[i]);
		if (port_init(port_id[i], mbuf_pool[socket], queue_count) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port[%d] %d\n", i, port_id[i]);
	}

	/* create event loop for the main thread */
	loop = ev_default_loop(EVFLAG_AUTO | EVFLAG_SIGNALFD);
	if (loop == NULL)
		return 1;

	/* set sigint/sigterm handlers */
	if (isatty(STDIN_FILENO)) {
		ev_signal_init(&ev_sigint, &sigint_hdl, SIGINT);
		ev_signal_start(loop, &ev_sigint);
	}
	ev_signal_init(&ev_sigterm, &sigterm_hdl, SIGTERM);
	ev_signal_start(loop, &ev_sigterm);

	tcp_init(&c);

	/* start all worker threads (but not us, return immediately) */
	rte_eal_mp_remote_launch(lcore_main, NULL, SKIP_MAIN);

	ret = rte_telemetry_register_cmd("/" PROG_NAME "/shutdown", shutdown_cmd, "Shuts down " PROG_NAME);
	printf("register shutdown returned %d for pid %d, tid %d\n", ret, getpid(), gettid());

	/* on this main core, you can run other service, like vty, ... */
	ev_run(loop, 0);

	/* program will exit. waiting for all cores */
	rte_eal_mp_wait_lcore();

#ifdef DEBUG
	printf("Got here\n");
#endif

	ev_loop_destroy(loop);

	return 0;
}
