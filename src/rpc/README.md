# RPC

How ngen processes talk to each other and to agents: JSON-RPC 2.0 over TCP on loopback, one endpoint per long-running process, found through
discovery files.

## Layout

| Part | Files | Depends on |
|---|---|---|
| Core | `core/rpcframe.*` (frames), `core/rpcprotocol.*` (JSON-RPC messages, error codes), `core/rpcsocket.*` (loopback TCP), `core/rpcserver.*` (an endpoint with an I/O thread that listens, connects out, or both), `core/rpcclient.*` (blocking client), `core/rpcdiscovery.*` (discovery files), `core/rpcresponder.*` (replies that can complete later), `core/rpcrecords.*` (named records: `introspect.list`, `introspect.get`) | the standard library and header-only nlohmann/json (`external/json`) |
| Engine layer | `rpcregistry.*` (methods, parameter schemas, `rpc.*` builtins, records as methods), `rpcendpoint.*` (server + discovery + main-thread dispatch) | the core, `src/obs` |
| Tool | `ngen-introspect` (`src/introspect/`) | the core |

The core stays free of engine code, so the build server (`src/build/`) can use it as well. Only `.cpp` files include `nlohmann/json.hpp`; headers
use `json_fwd.hpp`, with one exception: `rpcclient.h`, whose users are command-line tools that handle JSON anyway.

## Protocol

- **Frames.** `u32 jsonLength`, `u32 attachmentLength` (little-endian), the JSON text, then the attachment bytes. A message refers to its attachment
  by offset and length. Frames over 256 MB close the connection.
- **Messages.** JSON-RPC 2.0: requests (`id`, `method`, `params`), notifications (no `id`) and responses (`result` or `error`). Either side of a
  connection may send requests; responses are matched by id.
- **Errors.** `-32700` parse error, `-32600` invalid request, `-32601` unknown method, `-32602` invalid params (the message names the field),
  `-32603` internal error, `-32000` engine failures ("prim '/x' not found") with a message.
- **Version.** `rpc.version` returns `{protocol, kind}`; the protocol is `rpc::protocolVersion`.

## Discovery

Every endpoint writes `.ngen-discovery/<kind>-<pid>.json` in its working directory once it is listening: kind, pid, port, a label (the view's
scene path, the asset server's variant), protocol and start time. It removes the file on exit. Readers (`listRpcEndpoints`, `ngen-introspect list`) look in
their own working directory, so tools find each other when they run in the same directory, wherever their binaries are. Readers delete files whose
process is gone, so a crashed process leaves nothing behind after the next listing. Ports are chosen by the OS.

## Threading

- **The I/O thread** (`RpcServer`) accepts connections, reads frames, and writes queued replies. Any thread may queue a message. A caller never
  blocks on a slow client: a client whose unread replies pass 256 MB is dropped.
- **Connecting out.** `connect(port)` opens a connection that the same I/O thread serves like an accepted one, so requests, notifications and
  responses work the same both ways. `startWithoutListening()` runs the thread with no listening socket, for a process that only connects out, such
  as ngen-view's `AssetClient`. `RpcClient` stays the blocking client for command-line tools.
- **Pacing a sender.** `queuedBytes(connection)` says how much is queued and not yet written, and a drain handler (`setDrainHandler`) runs on the I/O
  thread after a write leaves a connection's queue at or below a threshold. The asset server streams packed data this way within a fixed window.
- **Dispatch** (`RpcEndpoint`). Requests are queued by the I/O thread and run by `drain()` on the thread that owns the data. ngen-view calls it on
  the main thread, where session commands run. Each drain runs calls for at most 2 ms (and at least one call), so a flood can't starve the frame.
- **Responders.** A handler gets an `RpcResponder`. It either answers at once, or keeps a copy and completes it later: a screenshot after its
  fence, a capture when its result arrives, a dump when its data is in. The first answer wins. Replies go straight to the I/O thread.

## Adding a method

1. Pick a name `area.verb` (`view.camera.set`, `introspect.memory`).
2. Register it with a `RpcMethodDesc`:
   - a one-line summary
   - typed parameters (`Bool`, `Int`, `Float`, `String`, `Vec3`, `Enum` with values, `Array`, `Object`), each required or optional with a
     description
   - a result description
3. Write the handler: parameters are checked before it runs, so it can read them with `get<>()`. Answer through the responder; use
   `rpc::appError` with a message for engine failures.
4. If it should also be a script verb, add a row to the verb table with a parser from the script's text to the parameters
   (`src/view/viewcommands.cpp`). Script lines and live calls then run the same code.
5. `rpc.describe` lists the new method automatically.

ngen-view's methods live in `src/view/`: `viewcommands.*` (the methods and the script verb table) and `viewdumps.*` (dumps and captures that
complete frames later, into a file, a reply or both).

## Records

Data a process gives for reading is a record, not a method: a name, a description and a producer in an `RpcRecords` table, served by
`introspect.list` and `introspect.get`. Clients show any record without knowing its type. The records and how to add one are in
[`src/introspect/README.md`](../introspect/README.md).

## Calling from the command line

`ngen-introspect` is the command-line client (`list`, `get`, `describe`, `call`); see [`src/introspect/README.md`](../introspect/README.md).

```sh
./ngen-cli introspect list                                   # live endpoints and their records
./ngen-cli introspect describe view                          # methods and schemas
./ngen-cli introspect get view status
./ngen-cli introspect call view introspect.gpuscene          # the joined GPU scene rows
./ngen-cli introspect call view:12345 view.camera.set '{"x": 5.3, "y": 11.3, "z": 1.2, "yaw": -169.8, "pitch": -0.2}'
```

## Observations

Engine category: `RpcListening` (port, discovery file), `RpcConnected` and `RpcDisconnected` (connection id), `RpcCall` per call (method, ms, ok,
error code).

## Known gaps

- Loopback only, no authentication: any local process can drive an endpoint.
- Request/response only; no subscriptions yet, so clients poll for anything that changes.
- Bulk results (screenshots, captures) travel as base64 or as files, not yet as attachments.
- `--no-rpc` turns ngen-view's endpoint off. It's compiled out of `gamerelease` (`NGEN_INTROSPECTION`).
