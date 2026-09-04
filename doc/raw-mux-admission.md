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
and drop the initial packet until a retransmission can be routed to the peer.
Authentication, exact-tuple replay and capacity policy belong to the application.
The gate itself does not authorize traffic or retain per-client state.

Unregister using the same address/port and a null callback. This waits for active
callbacks before the caller can free its context. If peers still exist, their
endpoint remains fail-closed; close peers before unregistering in normal shutdown.
After the final listener/agent is removed, the socket is released. Statistics
report received/rejected datagrams, registered agents and promoted tuple-map entries.

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

Tests use loopback UDP 49181/49182 and must run without other listeners there.
No Minecraft client, registration/control service, TURN or external STUN is involved.
