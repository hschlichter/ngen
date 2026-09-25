#pragma once

// Which introspection windows are open (Windows > Introspection). Owned by EditorUI; the
// menu and session verbs edit the same flags.
struct IntrospectionFlags {
    bool memory = false;
    bool capture = false;
    bool frameDebugger = false;
    bool gpuScene = false;
    bool counters = false;
    bool shaders = false;

    // RenderDoc: set by main when the in-app API is loaded; the Debug menu raises requests.
    bool renderDocAvailable = false;
    bool renderDocCaptureRequested = false;
    bool renderDocOpenRequested = false;
};
