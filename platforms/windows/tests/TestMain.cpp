#include "TestHarness.h"

// The whole suite is pure logic — coverage geometry, frame pacing, output
// sizing, index decoding — so it needs no display, no GPU, no Media Foundation
// session and no media files, and runs on a headless CI machine.
//
// What it deliberately does not cover, because none of it can be exercised
// without a real desktop session: the WorkerW parenting, the decode path, and
// the power notifications.
int main() { return livewall::test::runAll(); }
