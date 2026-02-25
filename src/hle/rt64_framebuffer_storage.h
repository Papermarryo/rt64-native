//
// RT64
//

#pragma once

#include "common/rt64_common.h"

#include <map>

namespace RT64 {
    struct FramebufferStorage {
        struct Handle {
            uint32_t fbPairIndex;
            RDPAddress address;
            uint32_t rdramIndex;
            uint32_t size;
        };

        uint32_t rdramUsed;
        std::vector<uint8_t> rdramData;
        std::vector<Handle> handleVector;

        FramebufferStorage();
        void reset();
        void store(uint32_t fbPairIndex, RDPAddress address, const uint8_t *data, uint32_t size);
        const Handle *get(uint32_t maxFbPairIndex, RDPAddress address) const;
        const uint8_t *getRDRAM(const Handle &handle) const;
    };
};