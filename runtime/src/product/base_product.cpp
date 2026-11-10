#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::BaseGame,
        "Resident Evil 4 Wii",
    };
    return descriptor;
}

} // namespace RuntimeProduct
