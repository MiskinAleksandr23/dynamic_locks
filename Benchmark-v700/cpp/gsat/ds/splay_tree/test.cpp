#include "catch.hpp"

#include <stress_test.h>
#include <unit_test.h>
#include <tree_builder.h>

#include "splay_tree.h"

template <typename Value>
class SplayTreeBuilder : public TreeBuilder<SplayTree<int, Value>, Value> {
public:
    using Tree = SplayTree<int, Value>;

    Tree* Build() override {
        return new Tree(this->GetNoValue());
    }
};

void StressTest(const tree_tests::StressTestConfig<int>& config) {
    tree_tests::StressTest(new SplayTreeBuilder<tree_tests::ValueType>(), config)();
}

TEST_CASE("insert_delete") {
    tree_tests::UnitTest(new SplayTreeBuilder<int>(), tree_tests::kInsertDeleteAction)();
}

TEST_CASE("small_stress") {
    StressTest(tree_tests::kSmallStressTestConfig);
}

TEST_CASE("mid_stress") {
    StressTest(tree_tests::kMidStressTestConfig);
}

TEST_CASE("big_dense_stress") {
    StressTest(tree_tests::kBigDenseStressTestConfig);
}

TEST_CASE("big_no_dense_stress") {
    StressTest(tree_tests::kBigNoDenseStressTestConfig);
}
