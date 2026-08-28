// TypeScript port of src/rpc_framing.h's Content-Length/JSON-RPC 2.0 wire
// framing -- the agent-control socket (src/agent_rpc.cpp) speaks this on
// one side; this MCP server (server.ts, via mep_client.ts) is the other.
// Kept as a hand-port rather than shared source since the two sides are
// different languages with no practical way to share a single file; see
// that header's own comment for why this framing (byte-count- rather than
// line-delimited) exists at all.

// A real Content-Length header is a couple dozen bytes; a real message
// body here is a small JSON-RPC call/response/event, not a bulk payload.
// Both caps are generous headroom over any legitimate use of this wire
// format -- see rpc_framing.h's identical caps and their own comment.
const MAX_HEADER_BYTES = 8 * 1024;
const MAX_BODY_BYTES = 64 * 1024 * 1024;

export function frameMessage(body: string): Uint8Array {
  const bodyBytes = new TextEncoder().encode(body);
  const headerBytes = new TextEncoder().encode(`Content-Length: ${bodyBytes.length}\r\n\r\n`);
  const out = new Uint8Array(headerBytes.length + bodyBytes.length);
  out.set(headerBytes, 0);
  out.set(bodyBytes, headerBytes.length);
  return out;
}

// Incremental byte-accumulating parser -- feed it chunks as they arrive
// from the socket via push(), and it calls onMessage for each complete
// frame found, exactly mirroring PumpRpcFrames' semantics (including
// draining more than one complete message per push() if that many
// happened to be sitting in the buffer at once). Throws on a fatal
// framing violation (no Content-Length header where one was expected, or
// an implausibly large header/body) -- same "once a byte-count-framed
// stream loses sync there's no way to resynchronize" reasoning as the
// C++ side; callers should treat that as fatal to the connection, not
// try to keep parsing.
export class RpcFramer {
  #buffer = new Uint8Array(0);

  push(chunk: Uint8Array, onMessage: (body: string) => void): void {
    const merged = new Uint8Array(this.#buffer.length + chunk.length);
    merged.set(this.#buffer, 0);
    merged.set(chunk, this.#buffer.length);
    this.#buffer = merged;

    for (;;) {
      const headerEnd = indexOfCrlfCrlf(this.#buffer);
      if (headerEnd < 0) {
        if (this.#buffer.length > MAX_HEADER_BYTES) {
          throw new Error("agent RPC framing: header never terminated (or garbage) within size cap");
        }
        return; // waiting for more bytes -- normal, not an error
      }
      const header = new TextDecoder().decode(this.#buffer.subarray(0, headerEnd));
      const match = /Content-Length:\s*(\d+)/i.exec(header);
      if (!match) {
        throw new Error(`agent RPC framing: no Content-Length header found in [${header}]`);
      }
      const expectedLen = Number(match[1]);
      if (!Number.isFinite(expectedLen) || expectedLen > MAX_BODY_BYTES) {
        throw new Error(`agent RPC framing: implausible Content-Length ${match[1]}`);
      }
      const bodyStart = headerEnd + 4;
      if (this.#buffer.length < bodyStart + expectedLen) return; // body incomplete, wait for more
      const body = new TextDecoder().decode(this.#buffer.subarray(bodyStart, bodyStart + expectedLen));
      this.#buffer = this.#buffer.slice(bodyStart + expectedLen);
      onMessage(body);
    }
  }
}

function indexOfCrlfCrlf(buf: Uint8Array): number {
  for (let i = 0; i + 4 <= buf.length; i++) {
    if (buf[i] === 13 && buf[i + 1] === 10 && buf[i + 2] === 13 && buf[i + 3] === 10) return i;
  }
  return -1;
}
