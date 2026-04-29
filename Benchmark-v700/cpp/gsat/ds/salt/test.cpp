#include "catch.hpp"

#include <stress_test.h>
#include <unit_test.h>
#include <tree_builder.h>

#include "salt.h"

template <typename Value, ClearPolicy CP>
class SALTBuilder : public TreeBuilder<SALT<int, Value, CP>, Value> {
public:
    using Tree = SALT<int, Value, CP>;

    SALTBuilder(int left, int right)
        : left_(left),
          right_(right) {
    }

    Tree* Build() override {
        return new Tree(this->GetNoValue(), left_, right_);
    }

private:
    int left_;
    int right_;
};

void StressTest(const tree_tests::StressTestConfig<int>& config) {
    tree_tests::StressTest(new SALTBuilder<tree_tests::ValueType, ClearPolicy::kRoot>(
                               config.GetMinKey(), config.GetMaxKey() + 1),
                           config)();
}

TEST_CASE("insert_delete") {
    tree_tests::UnitTest(new SALTBuilder<int, ClearPolicy::kRapid>(-100, 100),
                         tree_tests::kInsertDeleteAction)();
}

TEST_CASE("small_stress") {
    StressTest(tree_tests::kSmallStressTestConfig);
}

TEST_CASE("mid_stress") {
    StressTest(tree_tests::kMidStressTestConfig);
}

//TEST_CASE("big_dense_stress") {
//    StressTest(tree_tests::kBigDenseStressTestConfig);
//}
//
//TEST_CASE("big_non_dense_stress") {
//    StressTest(tree_tests::kBigNoDenseStressTestConfig);
//}
