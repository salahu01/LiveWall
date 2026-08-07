#include <cstdlib>
#include <cstring>
#include <string>

#include "Testing.h"

// `livewall_tests [suite]`. ctest registers one entry per suite so a failure
// names the area rather than the binary.
int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    return ::testing::runAll(filter);
}
