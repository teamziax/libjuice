# Contribution provenance

This maintained fork compares `nxs-dev` with the upstream mirror at `f09cf92522de0b619a63d55d007b5466c7c14ad1`.
The aggregate owned draft records the combined work; future external submissions
should separate independent fixes, acceptance APIs and lifecycle changes.

| Source | Contribution |
| --- | --- |
| Upstream [`f09cf925`](https://github.com/paullouisageneau/libjuice/commit/f09cf92522de0b619a63d55d007b5466c7c14ad1) | Baseline, including ICE-TCP, registry and build fixes. Replaces the former `5948a416` baseline. |
| Zulu `7c10b007` | Original credential checks and raw listener work. The credential checks remain; asynchronous acceptance replaces the packet callback design. |
| Zulu `9749544d`, `1b43ad56` | Earlier deferred packet processing and queue limits, now implemented as native-owned pending requests. |
| Consolidation and current changes | Async listener, authentication ordering, CRC race fix, username validation, regression tests, documentation and maintained-branch CI. |

Original author attribution, historical branches and verified backup bundles are preserved.

## Review follow-up, 8 September 2026

Index pending requests by ID and keep separate expiry, notification and attachment queues. Extend cancellation/reuse regressions and retain a reproducible queue-maintenance benchmark.
