# Pending-request queue maintenance

Separate expiry, notification and attachment queues let an idle receive iteration
inspect queue heads instead of scanning all retained requests. Requests are also
indexed by ID, so application decisions do not search the entire backlog.
Processing remains proportional to due expirations, notifications and attachments;
hash collisions and simultaneous attachment reservations still require bounded scans.

On 8 September 2026, the synthetic driver in `test/mux_pending_bench.c` compared
the original PR head `5498f67aff2092aa2bb868f74971ae7c076e5735` with these queue
changes. Median nanoseconds per empty prepare/process iteration across three
interleaved runs of 20,000 iterations were:

| Pending requests | Original PR | Indexed queues |
| --- | ---: | ---: |
| 0 | 88.6 | 85.2 |
| 256 | 5,788.8 | 84.9 |
| 1,024 | 40,899.4 | 85.6 |
| 4,096 | 470,850.1 | 88.1 |

The driver uses the actual mux implementation, with notifications already delivered,
unexpired requests, no agents and no received packets. Both versions used the same
driver and GCC 16.2.1 with `-O2` on an Intel Core i9-10885H Linux x86_64 host. The
host was not isolated from other workloads. The raw measurements are in
[`pending-performance-results.txt`](pending-performance-results.txt).

This measures queue bookkeeping only. It does not measure packet throughput,
message latency, connection establishment or the complete cost of admission. Those
require separate traffic tests with equivalent logging and build settings.

The extra list links increase each pending node from 3,016 to 3,056 bytes on this
build. At 4,096 requests, nodes occupy 12,517,376 bytes (about 11.94 MiB), before
hash tables, allocator overhead and binding metadata. The additional ID table uses
65,536 bytes at that limit on this host.

To run the current driver:

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DNO_TESTS=ON \
  -DPENDING_MUX_TESTS=ON -DENABLE_LOCALHOST_ADDRESS=ON
cmake --build build-bench --target mux-pending-bench
./build-bench/mux-pending-bench
```

For an equivalent baseline comparison, compile this driver's translation unit
against each checkout's `src` and `include` directories and static library with
`cc -O2 ... -pthread`. Define `BENCH_INDEXED_PENDING=1` only for the indexed version;
that flag frees the additional ID table after measurement and does not change the
timed loop. Keep the same compiler flags for both versions and report all runs.
