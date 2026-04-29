#pragma once

#include <iostream>
#include <cstdlib>

#include <test.h>
#include <validate.h>
#include <tree_builder.h>

#include "util/gen.h"
#include "map/map.h"

namespace tree_tests {
    using ValueType = const void *;

    template<typename Key>
    inline ValueType KeyToValue(const Key &key) {
        return &key;
    }

    constexpr ValueType kNoValue = nullptr;

    template<typename Key>
    class StressTestConfig {
    public:
        size_t GetOperationsCount() const {
            return operations_count_;
        }

        StressTestConfig &SetOperationsCount(size_t operations_count) {
            operations_count_ = operations_count;
            return *this;
        }

        Key GetMinKey() const {
            return min_key_;
        }

        StressTestConfig &SetMinKey(const Key &min_key) {
            min_key_ = min_key;
            return *this;
        }

        Key GetMaxKey() const {
            return max_key_;
        }

        StressTestConfig &SetMaxKey(const Key &max_key) {
            max_key_ = max_key;
            return *this;
        }

        size_t GetValidateOperationBound() const {
            return validate_operation_bound_;
        }

        StressTestConfig &SetValidateOperationBound(size_t validate_operation_bound) {
            validate_operation_bound_ = validate_operation_bound;
            return *this;
        }

        const uint32_t &GetSeed() const {
            return seed_;
        }

        StressTestConfig &SetSeed(uint32_t seed) {
            seed_ = seed;
            return *this;
        }

    private:
        size_t operations_count_;
        Key min_key_;
        Key max_key_;
        size_t validate_operation_bound_;
        uint32_t seed_;
    };

    template<typename TestTree, typename Key, typename Generator = RandomGenerator>
    class StressTest : public AbstractTest {
    public:
        StressTest(TreeBuilder<TestTree, ValueType> *test_tree_builder, StressTestConfig<Key> test_config)
                : test_tree_builder_(test_tree_builder), config_(test_config),
                  gen_(nullptr), tree_(nullptr), map_(nullptr) {
            test_tree_builder_->SetNoValue(kNoValue);
        }

        ~StressTest() override {
            delete test_tree_builder_;
        }

    protected:
        void SetUp() override {
            gen_ = new Generator(config_.GetSeed());
            tree_ = test_tree_builder_->Build();
            map_ = new Map<Key, ValueType>(test_tree_builder_->GetNoValue());
        }

        void TearDown() override {
            delete gen_;
            delete tree_;
            delete map_;
        }

        void Run() override {
            for (size_t op_number = 1; op_number <= config_.GetOperationsCount(); ++op_number) {
                // std::cout << op_number << std::endl;
                Operation operation = gen_->GenOperation();
                ApplyOperation(op_number, operation);
            }
        }

    private:
        void ApplyOperation(size_t op_number, Operation operation) {
            auto key = gen_->GenKey(config_.GetMinKey(), config_.GetMaxKey());
            switch (operation) {
                case Operation::kFind: {
                    REQUIRE(tree_->Find(key) == map_->Find(key));
                    break;
                }
                case Operation::kContains: {
                    REQUIRE(tree_->Contains(key) == map_->Contains(key));
                    break;
                }
                case Operation::kInsert: {
                    auto value = KeyToValue(key);
                    REQUIRE(tree_->Insert(key, value) == map_->Insert(key, value));
                    break;
                }
                case Operation::kDelete: {
                    REQUIRE(tree_->Delete(key) == map_->Delete(key));
                    break;
                }
            }
            if (op_number <= config_.GetValidateOperationBound()) {
                Validate(tree_, map_);
            }
        }

        TreeBuilder<TestTree, ValueType> *test_tree_builder_;
        StressTestConfig<Key> config_;

        RandomGenerator *gen_;
        TestTree *tree_;
        Map<Key, ValueType> *map_;
    };

    const auto kSmallStressTestConfig = StressTestConfig<int>()
            .SetSeed(99990001)
            .SetOperationsCount(10'000)
            .SetMinKey(1)
            .SetMaxKey(1000)
            .SetValidateOperationBound(100);

    const auto kMidStressTestConfig = StressTestConfig<int>()
            .SetSeed(2796203)
            .SetOperationsCount(1'000'000)
            .SetMinKey(1)
            .SetMaxKey(100'000)
            .SetValidateOperationBound(10'000);

    static constexpr uint32_t kBigStressTestSeed = 6'700'417;
    static constexpr size_t kBigStressTestOperationsCount = 100'000'000;

    const auto kBigNoDenseStressTestConfig = StressTestConfig<int>()
            .SetSeed(kBigStressTestSeed)
            .SetOperationsCount(kBigStressTestOperationsCount)
            .SetMinKey(1)
            .SetMaxKey(10'000'000)
            .SetValidateOperationBound(10'000);

    const auto kBigDenseStressTestConfig = StressTestConfig<int>()
            .SetSeed(kBigStressTestSeed)
            .SetOperationsCount(kBigStressTestOperationsCount)
            .SetMinKey(1)
            .SetMaxKey(100'000)
            .SetValidateOperationBound(1'000'000);

}  // namespace tree_tests

template<typename Tree>
class TreeBuilder<Tree, tree_tests::ValueType> {
public:
    virtual ~TreeBuilder() = default;
    virtual Tree *Build() = 0;

    tree_tests::ValueType GetNoValue() const {
        return tree_tests::kNoValue;
    }

    void SetNoValue(const tree_tests::ValueType &no_value) {
    }
};
