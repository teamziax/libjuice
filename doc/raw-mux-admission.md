# Raw UDP mux admission gate

`juice_mux_listen_raw(address, port, callback, user_ptr)` owns an endpoint without
creating an ICE agent. The callback sees the original datagram and source tuple
before STUN parsing, tuple lookup or map insertion. Returning false drops it;
returning true permits ordinary ICE processing and its integrity checks.
Only one raw or parsed listener may own an endpoint. Registry identity includes
the configured address as well as the UDP port.

The borrowed bytes/address expire at callback return. Callbacks run under the mux
lock: do not call juice APIs, create/destroy agents or block there. Validate bounded
input, atomically claim admission, queue bounded creation work on an owning thread,
and return false while the peer is being created.
Authentication, exact-tuple replay and capacity policy belong to the application.
The gate itself does not authorize traffic or retain per-client state.

Unregister using the same address/port and a null callback. This waits for active
callbacks before the caller can free its context. If peers still exist, their
endpoint remains fail-closed until a new raw listener is registered. Close peers
before unregistering in normal shutdown.
After the final listener/agent is removed, the socket is released. Statistics
report received/rejected raw-gated datagrams (including replays), registered agents
and promoted tuple-map entries. These are endpoint diagnostics for capacity and
resource-lifetime monitoring, not an authorization decision or application metrics.

The Linux regression tests reproduce legacy tuple promotion on forged integrity,
then prove rejection before promotion with zero/one agents; verify fail-closed
removal, exclusive ownership, distinct bind addresses, high port and cleanup.
The credential test reproduces a 257-character local ufrag silently truncating to
256 before the fix; both local and remote oversized credentials now fail explicitly.

```sh
cmake -S . -B build/raw-mux -DNO_TESTS=ON -DRAW_MUX_TESTS=ON -DENABLE_LOCALHOST_ADDRESS=ON
cmake --build build/raw-mux -j4
ctest --test-dir build/raw-mux --output-on-failure
```

Tests use loopback UDP 49181/49182/49183 and ephemeral ports and must run without other listeners there.
Tests need no external signalling, TURN or STUN service.

## Deferred first requests

Applications using lazy ICE-agent creation can call `juice_mux_replay` after setting
up the peer on an owning thread. The function copies one STUN Binding request and
its numeric source address/port into the endpoint's queue. This avoids waiting for
a remote retransmission after bounded admission work has completed. It is useful
for shared-port conferencing, device gateways and custom authenticated signalling.

Each endpoint retains at most 1024 requests of at most 2048 bytes. Enqueue returns
`JUICE_ERR_INVALID` for invalid arguments, unsupported packet size or message type;
`JUICE_ERR_NOT_AVAIL` for a missing listener/endpoint or a full queue; and
`JUICE_ERR_FAILED` for allocation or wakeup failure. Zero means the bytes were
copied, not that the peer accepted them. Queue overflow must remain an explicit
application admission failure or leave the remote to retransmit.

Replays run on the mux thread through the current raw gate and ordinary ICE
parsing/integrity checks; enqueueing never invokes the callback or ICE agent on the
caller thread. The application must revalidate any admission state in its gate.
Removing the listener drops queued requests before returning. Wakeups are coalesced
when the queue is already nonempty. Address identity uses the configured bind string
and port; use the same string for agents, listener management, stats and replay.

The deferred-request regression sends a single loopback request before creating an
agent, then receives an integrity-checked response after replay without any client
retransmission. A second case passes the gate but has a wrong ICE password and must
receive no successful response. Other regressions cover copied-buffer ownership,
queue capacity/removal, malformed raw bytes, exclusive ownership, the callback
removal barrier and endpoint reuse. The lifetime test deliberately suspends a
callback to observe removal; applications must not block inside their callback.

The credential-bounds change is independently useful and can be proposed without
the mux API. The raw gate/bind/lifetime API is a second proposal; deferred requests
are a third proposal stacked on it. None adds application signalling, authentication
policy or protocol parsing. No upstream equivalent for these raw/deferred APIs is
present in the baseline; standard ICE attribute configuration and parsed incoming
mux notification remain unchanged.
