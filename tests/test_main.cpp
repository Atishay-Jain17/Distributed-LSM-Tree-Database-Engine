#include "test_framework.h"

int main() {
    return ::lsm::test::TestRegistry::Instance().RunAll();
}
