#include "catch.hpp"

#include <stress_test.h>
#include <unit_test.h>
#include <tree_builder.h>

#include "ist.h"

template<typename Value>
class ISTBuilder : public TreeBuilder<IST<int, Value>, Value> {
public:
    using Tree = IST<int, Value>;

    ISTBuilder(double alpha, int left, int right) : alpha_(alpha), left_(left), right_(right) {
    }

    Tree *Build() override {
        return new Tree(this->GetNoValue(), alpha_, left_, right_);
    }

private:
    double alpha_;
    int left_;
    int right_;
};

void StressTest(double alpha, const tree_tests::StressTestConfig<int> &config) {
    auto ist_builder = new ISTBuilder<tree_tests::ValueType>(alpha, config.GetMinKey(), config.GetMaxKey() + 1);
    tree_tests::StressTest(ist_builder, config)();
}

TEST_CASE("insert_delete") {
    tree_tests::UnitTest(new ISTBuilder<int>(0.5, -1e9, 1e9), tree_tests::kInsertDeleteAction)();
}

TEST_CASE("small_stress") {
    StressTest(0.5, tree_tests::kSmallStressTestConfig);
}

TEST_CASE("mid_stress") {
    StressTest(0.75, tree_tests::kMidStressTestConfig);
}

TEST_CASE("big_dense_stress") {
    StressTest(0.5, tree_tests::kBigDenseStressTestConfig);
}

TEST_CASE("big_no_dense_stress") {
    StressTest(0.75, tree_tests::kBigNoDenseStressTestConfig);
}
