#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

#define RX_RING_SIZE 2048
#define RX_QUEUES 3
#define TX_RING_SIZE 4096
#define MBUFS 8191
#define MBUF_CACHE 256
#define MBUFSZ (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define LCORE_QUEUESZ 4194304
#define BURST_SIZE 32

#define RING_SIZE 1048576
#define MEMPOOL_CACHE_SIZE 256
#define MAX_PKT_BURST 32
#define RTE_TEST_RX_DESC_DEFAULT 4096  /**< Configurable number of RX ring descriptors */
#define RTE_TEST_TX_DESC_DEFAULT 16384 /**< Configurable number of TX ring descriptors */

#define NB_MBUF RTE_MAX(                    \
        (nb_ports * nb_rx_queue * nb_rxd +      \
         nb_ports * nb_lcores * MAX_PKT_BURST + \
         nb_ports * n_tx_queue * nb_txd +       \
         nb_lcores * MEMPOOL_CACHE_SIZE),       \
         (unsigned)8192)

static uint8_t nb_ports;
static volatile unsigned long packets_rx;
static volatile unsigned long packets_processed;
static uint64_t timer_period = 3;
static uint64_t timer_cycles;
static volatile char is_stop = 0;
unsigned int free_space = LCORE_QUEUESZ;
unsigned int free_space2 = LCORE_QUEUESZ;
struct rte_ring *queue;
struct rte_ring *packet_ring;

static const struct rte_eth_conf port_conf_default = {
    .rxmode =
        {
            .mq_mode = ETH_MQ_RX_RSS,
            .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
        },
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_TCP,
		},
	},
};

struct packet {
  int size;
  u_char *data;
};

int port_init(uint16_t port, struct rte_mempool *membuf_pool) {
  struct rte_eth_conf port_conf = port_conf_default;
  const uint16_t rx_rings = RX_QUEUES;
  const uint16_t tx_rings = 0;
  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;
  int ret;

  struct rte_eth_dev_info dev_info;
  if (!rte_eth_dev_is_valid_port(port)) return -1;

//   printf("Port is valid %u\n", port);

  ret = rte_eth_dev_info_get(port, &dev_info);
  if (ret != 0) {
    printf("Error during device info get port %u info %s\n", port,
           strerror(-ret));
    return ret;
  }

//   printf("Port device_info get success [%u]\n", port);

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
  if (ret != 0) return ret;

//   printf("Dev adjust rx-tx success [%u]\n", port);

  port_conf.rx_adv_conf.rss_conf.rss_hf &=
	  dev_info.flow_type_rss_offloads;
  if (port_conf.rx_adv_conf.rss_conf.rss_hf !=
		  port_conf.rx_adv_conf.rss_conf.rss_hf)
  {
	  printf("Port %u modified RSS hash function based on hardware support,"
			  "requested:%#" PRIx64 " configured:%#" PRIx64 "\n",
			  port,
			  port_conf.rx_adv_conf.rss_conf.rss_hf,
			  port_conf.rx_adv_conf.rss_conf.rss_hf);
  }

  ret = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
  if (ret != 0) return ret;
//   printf("rte_eth_dev_configure success [%u]\n", port);

  uint16_t q;
  for (q = 0; q < rx_rings; q++) {
//   printf("Going to initialize queue %u of port %u\n", port, port);
  ret = rte_eth_rx_queue_setup(port, q, nb_rxd, rte_eth_dev_socket_id(port),
                               NULL, membuf_pool);
  if (ret < 0) {
    printf("Error in rx queue_setup %u error %s\n", port, strerror(-ret));
    return ret;
  }
  }

//   printf("Port rx queue success [%u]\n", port);

  ret = rte_eth_dev_start(port);
  if (ret < 0) {
    printf("Error starting port %u  error %s\n", port, strerror(-ret));
    return ret;
  }

  printf("Port Configured and started [%u]\n", port);

  struct rte_ether_addr addr;
  ret = rte_eth_macaddr_get(port, &addr);
  if (ret != 0) {
    printf("Unable to get MAC of port %u, error %s", port, strerror(-ret));
    return ret;
  }

  ret = rte_eth_promiscuous_enable(port);
  if (ret != 0) return ret;

//   printf("Port promiscuous mode enabled! [%u]\n", port);

  return 0;
}

void print_stats(void) {
  struct rte_eth_stats st;
  printf("--------------------------------------------------------------\n");

  for (int p = 0; p < nb_ports; ++p) {
    rte_eth_stats_get(p, &st);
    printf("Port #%u: %lu received / %lu errors / %lu missed\n", p, st.ipackets,
           st.ierrors, st.imissed);
  }

  printf(
      "Rx packets: %lu \t Ring space: %u \t Packet ring: %u \t Packets "
      "processed %lu\n",
      packets_rx, free_space, free_space2, packets_processed);
  printf("--------------------------------------------------------------\n\n");
}

static inline int is_valid_ipv4_pkt(struct rte_ipv4_hdr *pkt, uint32_t link_len)
{
    /* From http://www.rfc-editor.org/rfc/rfc1812.txt section 5.2.2 */
    /*
     *      * 1. The packet length reported by the Link Layer must be large
     *           * enough to hold the minimum length legal IP datagram (20 bytes).
     *                */
    if (link_len < sizeof(struct rte_ipv4_hdr))
        return -1;
    /* 2. The IP checksum must be correct. */
    /* this is checked in H/W */
    /*
     *      * 3. The IP version number must be 4. If the version number is not 4
     *           * then the packet may be another version of IP, such as IPng or
     *                * ST-II.
     *                     */
    if (((pkt->version_ihl) >> 4) != 4)
        return -3;
    /*
     *      * 4. The IP header length field must be large enough to hold the
     *           * minimum length legal IP datagram (20 bytes = 5 words).
     *                */
    if ((pkt->version_ihl & 0xf) < 5)
        return -4;
    /*
     *      * 5. The IP total length field must be large enough to hold the IP
     *           * datagram header, whose length is specified in the IP header length
     *                * field.
     *                     */
    if (rte_cpu_to_be_16(pkt->total_length) < sizeof(struct rte_ipv4_hdr))
        return -5;
    return 0;
}

static int rx_packets(void *args) {
  uint16_t port = *(uint16_t *)args;
  // unsigned lcoreid = *(unsigned *)args;
  printf("Core %u processing rx packets on port %d \n", rte_lcore_id(), port);

  while (!is_stop) {
    struct rte_mbuf *bufs[BURST_SIZE];
    const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
    if (unlikely(nb_rx == 0)) continue;
    packets_rx += nb_rx;
    for (int i = 0; i < nb_rx; i++) {
      // Enqueue into rte_ring
      // rte_ring_enqueue_bulk(queue, (void **)&bufs[i], 1, &free_space);
     if(rte_pktmbuf_data_len(bufs[i]) > 0){
         struct packet *p = NULL;
         p = rte_malloc("packet_malloc", sizeof(struct packet), 0);
         p->size = rte_pktmbuf_data_len(bufs[i]);
         p->data = NULL;
         p->data = rte_malloc("packet_data", p->size, 0);
         rte_memcpy(p->data, rte_pktmbuf_mtod(bufs[i], unsigned char *), p->size);
         rte_ring_mp_enqueue_bulk(packet_ring, (void **)&p, 1, &free_space2);

         // printf("Packet of original: %d  copied with length %d\n",
         //      rte_pktmbuf_data_len(bufs[i]), p->size);


     }
     else{
     //printf("Error packet \n");
     }

     rte_pktmbuf_free(bufs[i]);

     // printf("Packet freed\n");
      // rte_free(p->data);
      // rte_free(p);a

    }
  }

  printf("Stopping rx Reader\n");
  //print_stats();

  return 0;
}

static int set_timer() {
  uint64_t diff_tsc, cur_tsc, prev_tsc, timer_tsc;

  prev_tsc = 0, timer_tsc = 0;
  while (!is_stop) {
    cur_tsc = rte_rdtsc();
    diff_tsc = cur_tsc - prev_tsc;
    if (timer_cycles > 0) { /* timer enabled */
      timer_tsc += diff_tsc;
      if (unlikely(timer_tsc >= timer_cycles)) { /* timeout */
        print_stats();
        timer_tsc = 0;
      }
    }
    prev_tsc = cur_tsc;
  }
}

//
//static int process_packets(void *args) {
//  unsigned lcoreid = *(unsigned *)args;
//  printf("Starting process on lcore %u\n", lcoreid);
//  struct rte_mbuf *mbuf[BURST_SIZE];
//  int nb, q;
//  packets_processed = 0;
//  // process packets
//  while (!is_stop) {
//    // Dequeue from rte_ring
//    nb = rte_ring_mc_dequeue_burst(queue, (void **)&mbuf, BURST_SIZE, NULL);
//    if (unlikely(nb == 0)) continue;
//
//    packets_processed += nb;
//    // Read packets from mbuf
//    for (q = 0; q < nb; q++) {
//      rte_pktmbuf_free(mbuf[q]);
//    }
//  }
//}

static int open_packets(void *args) {
  unsigned lcoreid = *(unsigned *)args;
  printf("Starting process on lcore %u\n", lcoreid);
  struct rte_mbuf *mbuf[BURST_SIZE];
  struct packet *arr_packets[BURST_SIZE];
  int nb, q;
  packets_processed = 0;
  // process packets
  while (!is_stop) {
     // printf("Checking burst\n");
    // Dequeue from rte_ring
    nb = rte_ring_mc_dequeue_burst(packet_ring, (void **)&arr_packets,
                                   BURST_SIZE, NULL);
    if (unlikely(nb == 0)) continue;
   // printf("packets dequeued\n");
    packets_processed += nb;
    // Read packets from mbuf
    for (q = 0; q < nb; q++) {
        // printf("packe dequeue size: %d with data: %.*s\n",
        // arr_packets[q]->size,
        //     10, arr_packets[q]->data);

        if(arr_packets[q]->data)
            rte_free(arr_packets[q]->data);
        if(arr_packets[q])
            rte_free(arr_packets[q]);
    }
    //rte_free(arr_packets);
  }
}

void exit_stats(int sig) {
  is_stop = 1;
  printf("Caught signal %d\n", sig);
  printf("Total received packets: %lu\n", packets_processed);
  exit(0);
}

int main(int argc, char *argv[]) {
  struct rte_mempool *membuf_pool;
  uint16_t portid;

  int ret = rte_eal_init(argc, argv);
  if (ret < 0) rte_exit(EXIT_FAILURE, "Error with EAL initializing");

  argc -= ret;
  argv += ret;
  printf("EAL configs set \n");

  timer_cycles = timer_period * rte_get_timer_hz();
  uint32_t nb_lcores = rte_lcore_count();
  nb_ports = rte_eth_dev_count_avail();
  uint16_t n_tx_queue = 0;
  uint16_t nb_rx_queue = 1;

  static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
  static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

  printf("Number of ports available %d\n", nb_ports);

  unsigned nb_mbuf = NB_MBUF;
  // pktmbuf_pool_create
  membuf_pool =
      rte_pktmbuf_pool_create("MBUF_POOL", nb_mbuf, MBUF_CACHE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    printf("pktmbuf pool done!\n");

  // mempool_create same as probe
 // unsigned hwsize = MBUFS * nb_ports;
 // unsigned swsize = LCORE_QUEUESZ;

 // struct rte_mempool *mempool2;
 // mempool2 = rte_mempool_create("mbuf_pool", hwsize + swsize, MBUFSZ, 32,
 //                               sizeof(struct rte_pktmbuf_pool_private),
 //                               rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init,
 //                               NULL, rte_socket_id(), 0);

  // mempool2 = rte_mempool_create("MBUF_POOL2",
  //                               swsize, sizeof(struct
  //                               rte_pktmbuf_pool_private), MBUF_CACHE,
  //                               RTE_MBUF_DEFAULT_BUF_SIZE,
  //                               NULL, NULL, NULL, NULL,
  //                               rte_socket_id(), 0);

  //if (mempool2 == NULL) rte_exit(EXIT_FAILURE, "Error in creating membuf pool");

  //printf("Mempool created successfully\n");

  RTE_ETH_FOREACH_DEV(portid) {
    if (port_init(portid, membuf_pool) != 0)
      rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 " \n", portid);
  }

  signal(SIGINT, exit_stats);

  //queue = rte_ring_create("RING 1", swsize, SOCKET_ID_ANY,
  //                       RING_F_SP_ENQ | RING_F_MC_RTS_DEQ);

  packet_ring = rte_ring_create("RING_PACKETS", RING_SIZE, SOCKET_ID_ANY,
                                RING_F_MP_RTS_ENQ | RING_F_MC_RTS_DEQ);

  unsigned lcoreid = 3;
  // RTE_LCORE_FOREACH_WORKER(lcoreid)
  for(int i=0;i<10 ;i++)
  {
    printf("Lcore starting remote process function\n");
    // rte_eal_remote_launch(process_packets, (void *)&lcoreid, lcoreid);
    lcoreid++;
    rte_eal_remote_launch(open_packets, (void *)&lcoreid,lcoreid);
  }
  // set_timer();
  portid = 0;
  uint32_t rxQueues = RX_QUEUES;
  RTE_ETH_FOREACH_DEV(portid) {
    printf("Starting rx on port %d\n", portid);
    for(int i=0;i< rxQueues;i++)
        rte_eal_remote_launch(rx_packets, (void *)&portid, ++lcoreid);
    // rte_eal_remote_launch(rx_packets, (void *)&portid, ++lcoreid);
  }

  rte_eal_remote_launch(set_timer, NULL, ++lcoreid);
  rte_eal_mp_wait_lcore();

  return 0;
}

