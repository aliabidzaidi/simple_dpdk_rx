# Simple Rx Using DPDK

Simple rx packets on dpdk ports
Tested on DPDK Version 20.11-rc4

## To Build & Run
```
gcc simple_rx.c $(pkg-config --cflags --libs --static libdpdk) -g -o simple_rx
./simple_rx
```


## TODO
- Add Throughput on Rx ports / Total bandwidth
- Add Rte_stats packets_missed