# Benchmark results

Generated: 2026-11-10 00:00 UTC
Host: Darwin 27.0.0 arm64
Apple M1 Pro

Loopback (client and exchange on the same host). Raw client output below; CSVs alongside.

## tcp_order
```
[tcp/order] n=50000 elapsed=1.480s throughput=33792 op/s bw=1.89 MB/s
  us  min=    17.8 p50=    27.8 p90=    37.2 p99=    62.4 p999=    97.2 max=   186.7 mean=    29.6 sd=     8.1
```

## tcp_throughput
```
[tcp/throughput] n=200000 elapsed=1.146s throughput=174464 op/s bw=9.77 MB/s
  us  min=    71.0 p50=   387.2 p90=   488.6 p99=   517.7 p999=   573.0 max=   718.2 mean=   366.8 sd=   104.8
```

## tcp_tick2trade
```
[tcp/tick2trade] n=20000 elapsed=2.000s throughput=9999 op/s bw=0.97 MB/s
  us  min=    17.4 p50=    26.8 p90=    39.3 p99=    65.9 p999=   103.5 max=   131.7 mean=    29.5 sd=     9.1
[tcp/tick2trade/order-leg] n=20000 elapsed=2.000s throughput=9999 op/s bw=0.00 MB/s
  us  min=    17.1 p50=    26.7 p90=    39.1 p99=    65.3 p999=   100.3 max=   131.5 mean=    29.3 sd=     9.0
[tcp/tick2trade/wire] n=20000 elapsed=2.000s throughput=9999 op/s bw=0.00 MB/s
  us  min=     9.9 p50=    16.7 p90=    23.7 p99=    40.4 p999=    65.4 max=   142.2 mean=    17.4 sd=     6.2
[tcp/tick2trade] seq_gaps=0 ticks_per_sec=9999
```

## grpc_order
```
[grpc/order] n=5000 elapsed=0.731s throughput=6841 op/s bw=0.21 MB/s
  us  min=    93.8 p50=   140.8 p90=   175.8 p99=   206.4 p999=   234.0 max=   275.8 mean=   146.1 sd=    21.0
```

## grpc_throughput
```
[grpc/throughput] n=20000 elapsed=0.810s throughput=24697 op/s bw=0.77 MB/s
  us  min=   138.7 p50=   318.6 p90=   395.0 p99=   470.8 p999=   548.6 max=  4324.7 mean=   323.2 sd=    95.8
```

## grpc_tick2trade
```
[grpc/tick2trade] n=2000 elapsed=1.336s throughput=1498 op/s bw=0.00 MB/s
  us  min=   138.5 p50=   222.6 p90=   288.0 p99=   396.8 p999=   864.6 max=  1488.9 mean=   233.1 sd=    59.2
[grpc/tick2trade/order-leg] n=2000 elapsed=1.336s throughput=1498 op/s bw=0.00 MB/s
  us  min=   138.5 p50=   222.6 p90=   287.9 p99=   396.8 p999=   864.6 max=  1488.9 mean=   233.1 sd=    59.2
[grpc/tick2trade/wire] n=2000 elapsed=1.336s throughput=1498 op/s bw=0.00 MB/s
  us  min=    62.2 p50=   100.1 p90=   132.0 p99=   193.4 p999=   808.4 max=  1180.1 mean=   106.5 sd=    46.3
[grpc/tick2trade] seq_gaps=0 ticks_per_sec=1498
```
