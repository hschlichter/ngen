#pragma once

#include "introspectsession.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// ngen-introspect log: every connected process's log events as JSON lines, merged by time.
//
//   log [--output=FILE] [--until-exit=TARGET] [--process=TARGET,…] [--category=A,…] [--type=A,…] [--severity=S] [--history]
//
// Events are taken from the time the command starts (--history: everything each process's ring still holds), from
// processes already running and from ones that start later. On stdout lines go out about a quarter of a second
// after they arrive, merged by time; history a late process brings can be older than lines already out. With
// --output the file is written at the end, sorted by time across processes (each process's own order kept).
// --severity=warning keeps warnings and errors, --severity=error errors only.
// --until-exit stops once the first process matching TARGET has connected and gone (its last events included);
// otherwise the command runs until SIGINT or SIGTERM. Returns the exit code.
auto runLogCommand(std::span<const std::string> args) -> int;

// One event as a JSONL line (no newline): ts_ns, process, thread, severity, category, type, name, text, fields.
auto logEventLine(const IntrospectSession::LogEvent& event) -> std::string;
