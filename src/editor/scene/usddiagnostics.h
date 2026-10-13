#pragma once

// Sends OpenUSD's own diagnostics — its errors, warnings and status messages — into the trace (category Scene, type
// UsdError, UsdWarning or UsdStatus), instead of OpenUSD printing them on stderr. Call once, before USD is used.
auto installUsdDiagnostics() -> void;
