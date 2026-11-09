#pragma once

#include <cstdlib>
#include <iostream>

// 不依賴 NDEBUG 的檢查巨集：失敗時印出位置並以 1 結束
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            std::exit(1);                                                            \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(((a) - (b)) <= (eps) && ((b) - (a)) <= (eps))
