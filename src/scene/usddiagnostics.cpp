#include "usddiagnostics.h"

#include "trace.h"

#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>

#include <format>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// The OpenUSD function the diagnostic came from: the event's name.
auto source(const TfDiagnosticBase& diagnostic) -> std::string {
    const char* function = diagnostic.GetContext().GetFunction();
    return function != nullptr ? function : "usd";
}

// "USD: <commentary> (in <function>)".
auto describe(const TfDiagnosticBase& diagnostic) -> std::string {
    const char* function = diagnostic.GetContext().GetFunction();
    if (function == nullptr) {
        return std::format("USD: {}", diagnostic.GetCommentary());
    }
    return std::format("USD: {} (in {})", diagnostic.GetCommentary(), function);
}

// While a delegate is registered OpenUSD prints nothing itself, so every diagnostic it doesn't mark quiet comes here.
class TraceDelegate : public TfDiagnosticMgr::Delegate {
public:
    void IssueError(const TfError& error) override {
        if (error.GetQuiet()) {
            return;
        }
        TRACE_ERROR("Scene", "UsdError", source(error)).text(describe(error));
    }

    void IssueFatalError(const TfCallContext& context, const std::string& message) override {
        TRACE_ERROR("Scene", "UsdFatalError", context.GetFunction() != nullptr ? context.GetFunction() : "usd").text(std::format("USD fatal error: {}", message));
        _UnhandledAbort();
    }

    void IssueStatus(const TfStatus& status) override {
        if (status.GetQuiet()) {
            return;
        }
        TRACE_EVENT("Scene", "UsdStatus", source(status)).text(describe(status));
    }

    void IssueWarning(const TfWarning& warning) override {
        if (warning.GetQuiet()) {
            return;
        }
        TRACE_WARNING("Scene", "UsdWarning", source(warning)).text(describe(warning));
    }
};

} // namespace

auto installUsdDiagnostics() -> void {
    // Lives as long as the process: OpenUSD keeps the pointer.
    static TraceDelegate delegate;
    TfDiagnosticMgr::GetInstance().AddDelegate(&delegate);
}
