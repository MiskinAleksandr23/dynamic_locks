#pragma once

#include <memory>

#include <stat.h>

namespace tree_tests {
    template<typename TestTree, typename MasterTree>
    void Validate(TestTree *test_tree, MasterTree *master_tree) {
        test_tree->Validate();

        auto test_root = test_tree->GetRoot();
        auto test_node_handler =
                std::unique_ptr<typename TestTree::NodeHandler>(test_tree->GetNodeHandler());

        auto master_root = master_tree->GetRoot();
        auto master_node_handler =
                std::unique_ptr<typename MasterTree::NodeHandler>(master_tree->GetNodeHandler());

        REQUIRE(GetNumKeys(test_node_handler.get(), test_root) ==
                GetNumKeys(master_node_handler.get(), master_root));

        REQUIRE(GetSumKeys(test_node_handler.get(), test_root) ==
                GetSumKeys(master_node_handler.get(), master_root));

        for (const auto &[k, v] : *master_tree) {
            REQUIRE(test_tree->Contains(k));
            REQUIRE(test_tree->Find(k) == v);
        }
    }
}  // namespace tree_tests
