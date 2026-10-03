#pragma once

#include "rpcdiscovery.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

// A process's name in the tool: "<kind>:<pid>", such as "view:12345".
auto processName(const RpcEndpointInfo& endpoint) -> std::string;

// The one endpoint a target names, or an error listing the candidates. A target is a kind ("view"), a kind and
// pid ("view:12345"), or a pid; a kind with several live endpoints fails.
auto resolveTarget(std::string_view target, const std::vector<RpcEndpointInfo>& endpoints) -> std::expected<RpcEndpointInfo, std::string>;
