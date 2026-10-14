#pragma once

#include <map>
#include <span>
#include <string>
#include <vector>

// The engine as a map: where each part runs (a lane: a process or a thread), what it is, what it reads from and hands to which other part,
// and where to look at it (its records, windows and README). Hand-written: it says what each part means, which a map derived from threads or
// profiler zones couldn't. A new part of the engine adds its node and edges here, with its records and window.
struct EngineMapNode {
    const char* id;
    const char* label;
    int lane; // index into engineMapLanes()
    int row;  // top to bottom, in the order data flows
    const char* description;
    std::vector<const char*> records; // records of the process the node runs in
    std::vector<const char*> windows; // window names ViewUI::setIntrospectionWindow opens
    const char* readme;
};

struct EngineMapEdge {
    const char* from;
    const char* to;
    const char* what; // what flows along it
};

auto engineMapLanes() -> std::span<const char* const>;
auto engineMapNodes() -> std::span<const EngineMapNode>;
auto engineMapEdges() -> std::span<const EngineMapEdge>;
auto engineMapNode(const std::string& id) -> const EngineMapNode*;

// Live numbers per node id, a few short lines each; the application fills them each frame while the map is open (and for the `map` record).
using EngineMapStats = std::map<std::string, std::vector<std::string>>;

// Asks the Engine Map window to open with a node selected: the "Map" link other windows draw next to their title line.
auto requestEngineMapFocus(const char* nodeId) -> void;
// The node asked for since the last call; empty when none was.
auto takeEngineMapFocus() -> std::string;
// A small "Map: <lane> > <node>" button that opens the Engine Map at the node: where the window's subject sits in the engine.
auto drawEngineMapLink(const char* nodeId) -> void;
