#include "harness.hpp"

// Run everything: ./halo_tests        One test or a group: ./halo_tests <substring>
int main(int argc, char** argv) {
    return halo::test::run(argc > 1 ? argv[1] : nullptr);
}
