# Benchmark results

Generated: 2026-11-10 04:40 UTC
Host: Darwin 27.0.0 arm64
Apple M1 Pro

Loopback (client and exchange on the same host). Raw client output below; CSVs alongside.

## tcp_order
```
[tcp/order] n=50000 elapsed=1.158s throughput=43186 op/s bw=2.42 MB/s
  us  min=    13.6 p50=    22.6 p90=    27.7 p99=    49.2 p999=    82.7 max=   124.8 mean=    23.1 sd=     6.3
```

## tcp_throughput
```
[tcp/throughput] n=200000 elapsed=1.092s throughput=183139 op/s bw=10.26 MB/s
  us  min=    74.1 p50=   366.0 p90=   444.0 p99=   523.8 p999=   639.6 max=   797.6 mean=   349.4 sd=    85.9
```

## tcp_tick2trade
```
[tcp/tick2trade] n=20000 elapsed=2.000s throughput=9999 op/s bw=0.97 MB/s
  us  min=    14.4 p50=    25.9 p90=    41.1 p99=    73.3 p999=   426.7 max=  1795.3 mean=    29.8 sd=    28.6
[tcp/tick2trade/order-leg] n=20000 elapsed=2.000s throughput=9999 op/s bw=0.00 MB/s
  us  min=    14.2 p50=    25.8 p90=    40.5 p99=    67.2 p999=   118.1 max=  1735.9 mean=    28.7 sd=    17.6
[tcp/tick2trade/wire] n=20000 elapsed=2.000s throughput=9999 op/s bw=0.00 MB/s
  us  min=     8.0 p50=    15.1 p90=    25.5 p99=    46.8 p999=   106.4 max=   221.9 mean=    17.1 sd=     9.5
[tcp/tick2trade] seq_gaps=0 ticks_per_sec=9999
```

## grpc_order
```
[grpc/order] n=5000 elapsed=0.695s throughput=7198 op/s bw=0.22 MB/s
  us  min=    95.6 p50=   136.7 p90=   163.3 p99=   203.9 p999=   292.2 max=  1304.8 mean=   138.9 sd=    32.8
```

## grpc_throughput
```
[grpc/throughput] n=20000 elapsed=0.722s throughput=27687 op/s bw=0.86 MB/s
  us  min=   126.2 p50=   282.9 p90=   358.4 p99=   440.6 p999=   593.0 max=  3118.8 mean=   288.3 sd=    77.7
```

## grpc_tick2trade
```
[grpc/tick2trade] n=2000 elapsed=1.363s throughput=1467 op/s bw=0.00 MB/s
  us  min=   120.1 p50=   226.7 p90=   322.8 p99=   544.5 p999=  3521.8 max=  4198.2 mean=   247.9 sd=   160.3
[grpc/tick2trade/order-leg] n=2000 elapsed=1.363s throughput=1467 op/s bw=0.00 MB/s
  us  min=   120.1 p50=   226.6 p90=   322.7 p99=   544.4 p999=  3521.7 max=  4198.1 mean=   247.9 sd=   160.3
[grpc/tick2trade/wire] n=2000 elapsed=1.363s throughput=1467 op/s bw=0.00 MB/s
  us  min=    57.4 p50=    99.8 p90=   140.9 p99=   243.5 p999=   853.3 max=  4064.0 mean=   109.3 sd=   101.1
[grpc/tick2trade] seq_gaps=0 ticks_per_sec=1467
```
