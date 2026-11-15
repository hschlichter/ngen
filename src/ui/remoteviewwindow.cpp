#include "remoteviewwindow.h"

#include "cvarwindow.h"
#include "profile.h"

#include <imgui.h>

namespace {

auto row(const char* label, const char* format, ...) -> void {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

} // namespace

auto drawRemoteViewWindow(bool& show, const RemoteViewStatus* status, const RemoteControlStatus* control) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Remote View", &show)) {
        ImGui::End();
        return;
    }
    if (status == nullptr) {
        ImGui::TextDisabled("This program has no remote view.");
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("The view's image as live video in a browser on another device. Open the address below there.");
    ImGui::Spacing();
    cvarEditor("Enabled", "remoteview.enabled");
    cvarEditor("Address", "remoteview.address");
    cvarEditor("Port", "remoteview.port");
    cvarEditor("Source", "remoteview.source");
    cvarEditor("Height", "remoteview.height");
    cvarEditor("Frames a second", "remoteview.fps");
    cvarEditor("Bitrate (kbit/s)", "remoteview.bitrateKbps");
    ImGui::Spacing();

    if (status->listening) {
        ImGui::TextUnformatted("Watch page (click to copy):");
        for (const auto& url : status->urls) {
            if (ImGui::Selectable(url.c_str(), false, ImGuiSelectableFlags_None)) {
                ImGui::SetClipboardText(url.c_str());
            }
        }
        if (status->urls.empty()) {
            ImGui::TextDisabled("this machine has no address but loopback");
        }
    } else if (status->enabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Not listening");
    } else {
        ImGui::TextDisabled("Off");
    }
    if (!status->lastError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", status->lastError.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Watcher", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("watcher", 2, ImGuiTableFlags_SizingFixedFit)) {
        row("State", "%s", videoWatcherStateName(status->watcher));
        row("Address", "%s", status->watcherAddress.empty() ? "-" : status->watcherAddress.c_str());
        row("Connected, gone", "%llu, %llu", (unsigned long long) status->connects, (unsigned long long) status->disconnects);
        const auto& report = status->report;
        if (report.received) {
            row("Browser", "%s", report.agent.c_str());
            row("Page says", "%s, %.1f s ago", report.state.c_str(), (double) (profile::now() - report.receivedNs) / 1e9);
            row("Decoded", "%llu frames, %llu dropped, %.0f a second", (unsigned long long) report.framesDecoded, (unsigned long long) report.framesDropped, report.framesPerSecond);
            row("Shown at", "%u x %u", report.width, report.height);
            if (report.latencyMs) {
                row("Latency", "%.0f ms, capture to display", *report.latencyMs);
            } else {
                row("Latency", "measured with the pattern as the source");
            }
        } else {
            row("Page", "no report yet");
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Control", ImGuiTreeNodeFlags_DefaultOpen)) {
        cvarEditor("Control", "remoteview.control");
        ImGui::TextWrapped("The watch page may call every method the RPC endpoint has, with no authentication. Takes effect when a watch page next connects.");
        if (control != nullptr && ImGui::BeginTable("control", 2, ImGuiTableFlags_SizingFixedFit)) {
            row("Channel", "%s", control->open ? "open" : (control->enabled ? "waiting for a watch page" : "off"));
            row("Calls", "%llu, %llu failed", (unsigned long long) control->calls, (unsigned long long) control->failed);
            row("Last method", "%s", control->lastMethod.empty() ? "-" : control->lastMethod.c_str());
            row("Largest message", "%llu bytes; %llu answers were larger", (unsigned long long) control->messageLimit, (unsigned long long) control->tooLarge);
            row("Dropped", "%llu messages (too many waiting)", (unsigned long long) control->dropped);
            row("Log", "%s, %llu events sent", control->logSubscribed ? "subscribed" : "not subscribed", (unsigned long long) control->logEvents);
            row("Runs", "on the main thread, after the RPC endpoint's calls");
            ImGui::EndTable();
        }
        const auto& report = status->report;
        if (report.received && !report.layout.empty()) {
            ImGui::TextDisabled("Watch page: %s layout, %s pointer", report.layout.c_str(), report.pointer.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Video", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("video", 2, ImGuiTableFlags_SizingFixedFit)) {
        row("Encoder", "%s", status->encoder.empty() ? "-" : status->encoder.c_str());
        row("Size", "%u x %u, from a frame of %u x %u", status->width, status->height, status->frameWidth, status->frameHeight);
        row("Rate", "%u frames a second, %u kbit/s", status->framesPerSecond, status->bitrate / 1000);
        row("Captured", "%llu", (unsigned long long) status->captured);
        row("Replaced", "%llu (a newer frame came before the encoder took it)", (unsigned long long) status->replaced);
        row("Encoded", "%llu, %llu keyframes", (unsigned long long) status->encoded, (unsigned long long) status->keyframes);
        row("Sent", "%llu, %.1f MB", (unsigned long long) status->sent, (double) status->sentBytes / (1024.0 * 1024.0));
        row("Keyframes asked for", "%llu", (unsigned long long) status->keyframeRequests);
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Where it sits, and what it costs", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("flow", 2, ImGuiTableFlags_SizingFixedFit)) {
        row("1. Render thread", "after the UI is drawn: the frame scaled and copied out on the GPU");
        row("2. Render thread", "once the GPU has finished that frame: copied to memory, %.2f ms", status->copyMs);
        row("3. Encode thread", "RGBA to Y'CbCr, %.2f ms", status->convertMs);
        row("4. Encode thread", "H.264, %.2f ms", status->encodeMs);
        row("5. Network", "sent %.1f ms after the frame was recorded", status->captureToSendMs);
        ImGui::EndTable();
    }
    ImGui::End();
}
