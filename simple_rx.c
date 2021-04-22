#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 0
#define MBUFS 8191
#define MBUF_CACHE 250
#define BURST_SIZE 32

static uint8_t nb_ports;
static unsigned long packets_processed;
static uint64_t timer_period = 2;
static uint64_t timer_cycles;
struct rte_ring *queue;

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

int port_init(uint16_t port, struct rte_mempool *membuf_pool)
{
  struct rte_eth_conf port_conf = port_conf_default;
  const uint16_t rx_rings = 1;
  const uint16_t tx_rings = 0;
  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;
  int ret;

  struct rte_eth_dev_info dev_info;
  if (!rte_eth_dev_is_valid_port(port))
    return -1;

  printf("Port is valid %u\n", port);

  ret = rte_eth_dev_info_get(port, &dev_info);
  if (ret != 0)
  {
    printf("Error during device info get port %u info %s\n", port, strerror(-ret));
    return ret;
  }

  printf("Port device_info get success [%u]\n", port);

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
  if (ret != 0)
    return ret;

  printf("Dev adjust rx-tx success [%u]\n", port);

  ret = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
  if (ret != 0)
    return ret;
  printf("rte_eth_dev_configure success [%u]\n", port);

  uint16_t q;
  for (q = 0; q < rx_rings; q++)
  {
    printf("Going to initialize queue %u of port %u\n", q, port);
    ret = rte_eth_rx_queue_setup(port, q, nb_rxd, rte_eth_dev_socket_id(port), NULL, membuf_pool);
    if (ret < 0)
    {
      printf("Error in rx queue_setup %u error %s\n", port, strerror(-ret));
      return ret;
    }
  }

  printf("Port rx queue success [%u]\n", port);

  ret = rte_eth_dev_start(port);
  if (ret < 0)
  {
    printf("Error starting port %u  error %s\n", port, strerror(-ret));
    return ret;
  }

  printf("Port started [%u]\n", port);

  struct rte_ether_addr addr;
  ret = rte_eth_macaddr_get(port, &addr);
  if (ret != 0)
  {
    printf("Unable to get MAC of port %u, error %s", port, strerror(-ret));
    return ret;
  }

  ret = rte_eth_promiscuous_enable(port);
  if (ret != 0)
    return ret;

  printf("Port promiscuous mode enabled! [%u]\n", port);

  return 0;
}

void print_stats(void)
{
  struct rte_eth_stats st;
  printf("--------------------------------------------------------------\n");

  for (int p = 0; p < nb_ports; ++p)
  {
    rte_eth_stats_get(p, &st);
    printf("Port #%u: %lu received / %lu errors / %lu missed\n", p, st.ipackets, st.ierrors, st.imissed);
  }

  printf("Packets processed %lu\n", packets_processed);
  printf("--------------------------------------------------------------\n\n");
}

void rx_packets()
{
  uint16_t port;
  uint64_t diff_tsc, cur_tsc, prev_tsc, timer_tsc;

  prev_tsc = 0, timer_tsc = 0;
  printf("Core %u processing rx packets\n", rte_lcore_id());

  for (;;)
  {
    RTE_ETH_FOREACH_DEV(port)
    {
      struct rte_mbuf *bufs[BURST_SIZE];
      const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);

      cur_tsc = rte_rdtsc();
      diff_tsc = cur_tsc - prev_tsc;

      if (timer_cycles > 0)
      { /* timer enabled */
        timer_tsc += diff_tsc;
        if (unlikely(timer_tsc >= timer_cycles))
        { /* timeout */
          print_stats();
          timer_tsc = 0;
        }
      }
      prev_tsc = cur_tsc;

      if (unlikely(nb_rx == 0))
        continue;

      for (int i = 0; i < nb_rx; i++)
      {
        packets_processed++;
        rte_pktmbuf_free(bufs[i]);
      }

    }
  }
}

void exit_stats(int sig)
{
  printf("Caught signal %d\n", sig);
  printf("Total received packets: %lu\n", packets_processed);
  exit(0);
}

int main(int argc, char *argv[])
{
  struct rte_mempool *membuf_pool;
  uint16_t portid;

  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "Error with EAL initializing");

  argc -= ret;
  argv += ret;
  printf("EAL configs set \n");

  timer_cycles = timer_period * rte_get_timer_hz();

  nb_ports = rte_eth_dev_count_avail();
  printf("Number of ports available %d\n", nb_ports);

  membuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", MBUFS * nb_ports, MBUF_CACHE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

  if (membuf_pool == NULL)
    rte_exit(EXIT_FAILURE, "Error in creating membuf pool");

  printf("Mempool created successfully\n");

  RTE_ETH_FOREACH_DEV(portid)
  {
    if (port_init(portid, membuf_pool) != 0)
      rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 " \n", portid);
  }

  signal(SIGINT, exit_stats);
  rx_packets();

  return 0;
}