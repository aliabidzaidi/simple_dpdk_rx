# Simple Rx Using DPDK

Simple rx packets on dpdk ports. Tested on DPDK Version 20.11-rc4
## Program Structure
Uses a rte_ring between rx_packets() and process_packets(). Process packets starts on a different lcore, whereas rx_packets operates on main thread.
```
                                      LCORE                    LCORE
   ┌───────────────┐                   ┌───────┐             ┌───────────┐
   │               │     Enqueue       │       │             │           │
   │           Port├──────────────────►│       │             │           │
   │               │                   │       │             │           │
   │               │                   │ RTE   │   Dequeue   │           │
   │ NIC           │                   │  RING │◄────────────┤ Process   │
   │               │                   │       │             │  Packets  │
   │               │                   │       │             │           │
   │           Port├──────────────────►│       │             │           │
   │               │                   │       │             │           │
   └───────────────┘                   └───────┘             └───────────┘
```

## To Build & Run
```
gcc simple_rx.c $(pkg-config --cflags --libs --static libdpdk) -g -o simple_rx
./simple_rx
```


## TODO
- Add Throughput on Rx ports / Total bandwidth
