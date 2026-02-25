//
// RT64
//

#pragma once

namespace RT64 {
    struct DisplayList {
        uint64_t w0;
        uint64_t w1;

        DisplayList();
        uint64_t p0(uint8_t pos, uint8_t bits) const;
        uint64_t p1(uint8_t pos, uint8_t bits) const;
    };
};