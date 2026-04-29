#pragma once

namespace tree_tests {
    class AbstractTest {
    public:
        virtual ~AbstractTest() = default;

        void operator()() {
            SetUp();
            try {
                Run();
            } catch (...) {
                TearDown();
                throw;
            }
            TearDown();
        }

    protected:
        virtual void SetUp() = 0;
        virtual void TearDown() = 0;
        virtual void Run() = 0;
    };

}  // namespace tree_tests
