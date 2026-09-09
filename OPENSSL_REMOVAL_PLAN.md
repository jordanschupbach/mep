# OpenSSL Removal Plan

**Verdict: keep.** No phases below -- recorded in
[DEPENDENCIES.md](DEPENDENCIES.md)'s inventory for completeness, not
because removal is planned.

## What it's used for

`src/collab_websocket.cpp` uses OpenSSL for three distinct things
(confirmed by reading the file, not just its own comment):

1. **Real TLS**, gated on the URL scheme (`const bool secure =
   url.rfind("wss://", 0) == 0`, only `SSL_CTX_new`/`SSL_new`/etc. when
   true): `SSL_CTX_new(TLS_client_method())`,
   `SSL_CTX_set_default_verify_paths`/`SSL_CTX_set_verify(..., SSL_VERIFY_PEER, ...)`,
   SNI via `SSL_set_tlsext_host_name`, hostname verification via
   `SSL_set1_host`, the handshake (`SSL_connect`) and its result check
   (`SSL_get_verify_result(...) != X509_V_OK`), then `SSL_read`/`SSL_write`
   for the encrypted byte stream.
2. **A cryptographically secure RNG**, unconditionally on every
   connection regardless of scheme: `RAND_bytes(nonce, sizeof(nonce))`
   for the WebSocket handshake's `Sec-WebSocket-Key` nonce (RFC 6455
   section 1.3).
3. **SHA-1**, also unconditional: the WebSocket upgrade handshake's
   `Sec-WebSocket-Accept` header (`SHA1(...)` over `key +
   "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"` -- a protocol-framing hash,
   not a security boundary on its own).

The file's own top comment undersold this slightly ("needs SHA-1" reads
as if that's the whole reason) -- (1) is the real, load-bearing use, and
(2) matters too (see below).

## Why not

(1) is a hard no on its own: rolling your own TLS implementation is the
textbook example of a security anti-pattern, not a reasonable "in-house
version" target. TLS's complexity is inseparable from its correctness
requirements -- certificate chain validation, hostname verification,
cipher suite negotiation, protocol version handling, padding-oracle and
timing-side-channel resistance in the primitives themselves -- and
getting any of it subtly wrong doesn't produce a visible bug the way a
misaligned glyph does; it produces a silent security hole that only
shows up when someone exploits it. OpenSSL (or a comparable audited
library -- BoringSSL, mbedTLS, etc.) is the correct dependency to keep
here regardless of how small mep's own `collab_websocket.cpp` is.

(3) alone -- just the `SHA1()` call -- *would* be a small, safe,
well-specified ~100-line algorithm to hand-write (SHA-1 for protocol
framing, not for any security property, is exactly the kind of "well-
understood leaf algorithm" that made stb_truetype/miniaudio reasonable
candidates). (2) is murkier: a nonce here isn't protecting anything
cryptographically (RFC 6455's `Sec-WebSocket-Key` exists to stop a
misbehaving proxy from replaying a cached response, not as a security
control), so a lower-quality PRNG would still satisfy the *protocol*,
but reaching for a hand-rolled RNG instead of an audited CSPRNG is the
kind of substitution that's easy to regret later if that nonce's
purpose ever expands. Either way, replacing (2)/(3) doesn't reduce the
actual dependency footprint: OpenSSL stays linked (and initialized)
for TLS the moment any `wss://` URL is used, which is mep's own
documented default expectation for real collaboration use, not an edge
case. Trading a small amount of risk (hand-rolled hash/RNG in a real
network protocol handshake) for zero build/link/dependency-count
benefit isn't worth it in isolation.

## If this ever gets revisited

If mep ever dropped `wss://` support entirely (collaboration only ever
running over plain `ws://` on a trusted local network), OpenSSL could be
reconsidered -- and at that point, replacing the `RAND_bytes`/`SHA1`
calls would be the one remaining small win. Not the case today: `wss://`
is a first-class, presumably-expected-to-be-used option
(`collab_websocket.cpp`'s scheme check treats it as equally valid to
`ws://`, not as an unused/vestigial code path).
