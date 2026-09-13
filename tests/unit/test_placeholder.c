// Phase 0 placeholder: proves the host build, the harness and CTest are wired.
// Replaced by per-section tests from Phase 3.

#include "test.h"

TEST(harness_counts_checks) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(0x9C, 0x9C);
}

int main(void) {
    RUN(harness_counts_checks);
    return TEST_RESULT();
}
