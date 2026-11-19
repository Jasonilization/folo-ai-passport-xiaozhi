// Portable C FixedMul (GBA used fixeddiv.s; the C3 is RISC-V, so use C).
#include "m_fixed.h"

fixed_t CONSTFUNC FixedMul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}
