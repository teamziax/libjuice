# Accept incoming ICE connections asynchronously

A shared UDP endpoint can receive a STUN request before the application has
created its ICE agent. Use `juice_mux_listen_pending` when deciding whether to
accept the connection requires application work on another thread.

The listener retains the first Binding request and reports its username fragments,
source address and source port. The callback receives a process-unique request ID.
Copy the metadata, queue the decision and return promptly. The callback runs on
the mux thread, outside its registry lock. Packet bytes stay inside libjuice.

To accept a request:

1. Obtain the connection's ICE credentials through your application protocol.
2. Call `juice_mux_verify_request` with the request ID and local ICE password.
   This checks the original STUN MESSAGE-INTEGRITY before an agent is allocated.
3. Create and configure a mux agent for the same bind address and port. Set its
   local and remote ICE credentials, then gather candidates.
4. Install the application's callbacks before calling `juice_mux_attach_request`.
   The agent must match both username fragments and the verified local password.

Attachment schedules the retained request on the mux thread. The first STUN
response does not require a client retransmission or application packet injection.
The caller owns the agent and must destroy it if preparation or attachment fails.
Use `juice_mux_reject_request` to cancel a pending or verified request.

Duplicates from the same source address, port and username fragments share one
pending request. They do not create additional callbacks, replace the first packet,
or extend the deadline. A failed integrity check discards the request; a later
attempt receives a new ID. After attachment, traffic on that authenticated source
address and port goes directly to the agent. New source addresses require another
application decision, even if they present a known username fragment.

The default limit is 256 pending requests with a five-second timeout. Configuration
allows at most 4096 requests and 30 seconds. Each request retains at most 2048
packet bytes. Duplicate lookup uses a bounded hash table. At most 64 notifications
are delivered per receive-loop iteration so timers and established agents also
make progress. The callback must still return promptly.

Stop by calling `juice_mux_listen_pending` with a null callback and the original
bind address and port. Stop cancels unaccepted requests and waits for active metadata
callbacks to return. The callback context can then be freed. Accepted agents continue running, including processing their retained first
request; new sources remain blocked. Stopping from inside the listener's
own callback returns an error. Late decisions cannot apply to a replacement
listener because request IDs are never reused. Keep the callback's owner alive
until stop returns, and coordinate concurrent stop calls in that owner.

The existing `juice_mux_listen` callback remains available for applications using
its synchronous notification contract. An endpoint can have one legacy or pending
listener. `juice_mux_get_stats` reports socket packets, rejections, registered
agents, authenticated address mappings, pending requests, notifications and
duplicates without adding a callback to ordinary traffic.

Build the loopback regressions with:

```sh
cmake -B build -DNO_TESTS=ON -DPENDING_MUX_TESTS=ON -DENABLE_LOCALHOST_ADDRESS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests cover a first request sent once, delayed acceptance, duplicate handling,
integrity failure before agent creation, verification before legacy address
mapping, request limits, expiry, listener replacement and cancellation while a
callback is active. They use local UDP sockets and no external signalling service.
