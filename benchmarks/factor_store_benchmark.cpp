#include "factor_store/factor_store.h"

#include <iostream>

int main() {
    const factor_store::FactorStore store;
    static_cast<void>(store);
    std::cout << "FactorStore benchmark placeholder\n";
    return 0;
}

