#pragma once

#include <functional>
#include <limits>

#include <error.h>
#include <test.h>
#include <validate.h>

namespace tree_tests {

    template<typename Key, typename Value>
    class MockTree {
    public:
        virtual Value Find(const Key &key) = 0;
        virtual bool Contains(const Key &key) = 0;
        virtual Value Insert(const Key &key, const Value &value) = 0;
        virtual Value Delete(const Key &key) = 0;
        virtual void EnableValidation() = 0;
        virtual void DisableValidation() = 0;
    };

    template<typename Key, typename Value>
    using Action = std::function<void(MockTree<Key, Value> &)>;

    template<typename TestTree, typename Key, typename Value>
    class UnitTest : public AbstractTest, public MockTree<Key, Value> {
    public:
        UnitTest(TreeBuilder<TestTree, Value> *test_tree_builder, Action<Key, Value> action)
                : test_tree_builder_(test_tree_builder), action_(action), validation_enabled_(true) {
            test_tree_builder->SetNoValue(std::numeric_limits<int>::min());
        }

        ~UnitTest() override {
            delete test_tree_builder_;
        }

        Value Find(const Key &key) override {
            auto result = test_tree_->Find(key);
            REQUIRE(result == map_->Find(key));
            ValidateTree();
            return result;
        }

        bool Contains(const Key &key) override {
            auto result = test_tree_->Contains(key);
            REQUIRE(result == map_->Contains(key));
            ValidateTree();
            return result;
        }

        Value Insert(const Key &key, const Value &value) override {
            Check(value != test_tree_builder_->GetNoValue());
            auto result = test_tree_->Insert(key, value);
            REQUIRE(result == map_->Insert(key, value));
            ValidateTree();
            return result;
        }

        Value Delete(const Key &key) override {
            auto result = test_tree_->Delete(key);
            REQUIRE(result == map_->Delete(key));
            ValidateTree();
            return result;
        }

        void EnableValidation() override {
            validation_enabled_ = true;
        }

        void DisableValidation() override {
            validation_enabled_ = false;
        }

    protected:
        void SetUp() override {
            test_tree_ = test_tree_builder_->Build();
            map_ = new Map<Key, Value>(test_tree_builder_->GetNoValue());
            validation_enabled_ = true;
        }

        void TearDown() override {
            delete test_tree_;
            delete map_;
        }

        void Run() override {
            action_(*this);
        }

    private:
        void ValidateTree() {
            if (validation_enabled_) {
                Validate(test_tree_, map_);
            }
        }

        TreeBuilder<TestTree, Value> *test_tree_builder_;
        Action<Key, Value> action_;

        TestTree *test_tree_;
        Map<Key, Value> *map_;
        bool validation_enabled_;
    };

    const Action<int, int> kInsertDeleteAction = [](MockTree<int, int> &mock_tree) {
        for (int key = 0; key < 50; ++key) {
            mock_tree.Insert(key, key + 5);
        }
        for (int key = 55; key >= -5; --key) {
            mock_tree.Delete(key);
        }
    };
}  // namespace tree_tests
