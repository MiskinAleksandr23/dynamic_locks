#include "catch.hpp"

#include <stress_test.h>
#include <unit_test.h>

#include "tree_builder.h"

#include "btree.h"

template<typename Value, int kMinKeys>
class BTreeBuilder : public TreeBuilder<BTree<int, Value, kMinKeys>, Value> {
public:
    using Tree = BTree<int, Value, kMinKeys>;

    Tree *Build() override {
        return new Tree(this->GetNoValue());
    }
};

template<int kMinKeys>
void StressTest(const tree_tests::StressTestConfig<int> &config) {
    tree_tests::StressTest(new BTreeBuilder<tree_tests::ValueType, kMinKeys>(), config)();
}

TEST_CASE("insert_delete") {
    tree_tests::UnitTest(new BTreeBuilder<int, 4>(), tree_tests::kInsertDeleteAction)();
}

TEST_CASE("small_stress") {
    StressTest<3>(tree_tests::kSmallStressTestConfig);
}

TEST_CASE("mid_stress") {
    StressTest<7>(tree_tests::kMidStressTestConfig);
}

TEST_CASE("big_dense_stress") {
    StressTest<12>(tree_tests::kBigDenseStressTestConfig);
}

TEST_CASE("big_non_dense_stress") {
    StressTest<16>(tree_tests::kBigNoDenseStressTestConfig);
}
