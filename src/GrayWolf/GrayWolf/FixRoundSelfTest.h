#pragma once

#include <string>

namespace FixRound
{
    // Self-tests of the M1 fix round's pure logic (LayerBits, the kept book's extras, the pool-coverage retry, image header facts, the plan arithmetic)
    int SelfTest(std::string& Failures);
}
