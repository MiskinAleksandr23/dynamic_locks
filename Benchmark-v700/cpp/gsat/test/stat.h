#pragma once

#include <utility>
#include <cstddef>

template<typename NodeHandler, typename R>
R GatherStat(NodeHandler *handler, typename NodeHandler::NodePtrType node,
             R (NodeHandler::*stat)(typename NodeHandler::NodePtrType), R default_stat_value) {
    if (!handler || !node) {
        return default_stat_value;
    }

    R result = (*handler.*stat)(node);

    if (!handler->IsLeaf(node)) {
        auto it = handler->GetChildIterator(node);
        while (it.HasNext()) {
            auto child = it.Next();
            result += GatherStat(handler, child, stat, default_stat_value);
        }
    }

    return result;
}

template<typename NodeHandler>
int64_t GetNumKeys(NodeHandler *handler, typename NodeHandler::NodePtrType node) {
    return GatherStat<NodeHandler, int64_t>(handler, node, &NodeHandler::GetNumKeys, 0);
}

template<typename NodeHandler>
int64_t GetSumKeys(NodeHandler *handler, typename NodeHandler::NodePtrType node) {
    return GatherStat<NodeHandler, int64_t>(handler, node, &NodeHandler::GetSumKeys, 0);
}
