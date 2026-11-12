# os

What the engine asks of the operating system, behind one header (`os.h`) with one implementation per OS: `oslinux.cpp` and `oswindows.cpp`.
`build.cpp` compiles each only on its platforms (`for_source(...).only_on(...)`). The interface is the standard library only, so no OS header reaches
the code that uses it.

| Function | Linux | Windows |
|---|---|---|
| `processId()` | `getpid` | `GetCurrentProcessId` |
| `processAlive(pid)` | `kill(pid, 0)` | `OpenProcess` + `GetExitCodeProcess` |
| `executablePath()` | `/proc/self/exe` | `GetModuleFileNameW` |
| `residentBytes()` | `/proc/self/statm` | `GetProcessMemoryInfo` (working set) |
| `setThreadName(name)` | `pthread_setname_np` (15 characters) | `SetThreadDescription` |
| `blockStopSignals()`, `waitForStopSignal()` | SIGINT and SIGTERM, blocked and `sigwait`ed | Ctrl+C, Ctrl+Break, console close (`SetConsoleCtrlHandler`) |
| `runProcess(argv)` | `posix_spawnp`, stdout and stderr on one pipe | `CreateProcessW`, the child inheriting only its pipe |
| `runProcessInteractive(program, args)` | fork, exec, wait | `CreateProcessW` on the same console, wait |
| `executableSuffix` | `""` | `".exe"` |

## Where it sits

Below everything: `profile` links it (thread names, resident memory), so every program that links `profile` links `os` too, as programs link
their archives directly (`build.cpp`). Its users:

- The asset server: its stop signal, its own path (the variant comes from the binary's directory), and the packers it runs (`runProcess`).
- `ngen-packer-shader`: runs `glslc` from `PATH` (`runProcess`, no shell).
- RPC discovery: a discovery file whose process is gone is stale (`processAlive`); endpoints and trace streams are named by `processId`.
- `ngen-view`, `ngen-pack`, `ngen-packdump`, `ngen-worldcheck`: the variant from their own path.
- `ngen-cli`: its repository root, and the tools it forwards to.

## Rules

- A function goes here when its implementation differs by OS and more than one library needs it. Code with a larger OS-specific part keeps it in its
  own library, split the same way: the RPC sockets (`src/rpc/core/rpcsocketlinux.cpp`, `rpcsocketwindows.cpp`), the asset server's file watcher
  (`src/asset/server/filewatcherlinux.cpp`, `filewatcherwindows.cpp`).
- `runProcess` is safe from several threads at once. On Windows a child inherits only the write end of its own pipe
  (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`); otherwise a child started at the same moment would hold another's pipe open and its reader would wait
  for both.

## Visibility

None of its own: what it reports reaches the records of its users (the resident memory in `profile` and the status bar, process ids in every
endpoint's discovery entry and trace stream name).
