//
// RT64
//

#pragma once

#include "common/rt64_common.h"
#include "rt64_framebuffer_manager.h"
#include "rt64_projection.h"

namespace RT64 {
    struct FramebufferPair {
        enum class FlushReason {
            None,
            SamplingFromColorImage,
            SamplingFromDepthImage,
            ColorImageChanged,
            DepthImageChanged,
            ProcessDisplayListsEnd
        };

        struct {
            RDPAddress address = 0;
            uint8_t fmt = 0;
            uint8_t siz = 0;
            uint16_t width = 0;
            bool formatChanged = false;
        } colorImage;

        struct {
            RDPAddress address = 0;
            bool formatChanged = false;
        } depthImage;

        struct {
            bool clearDepthOnly : 1;
        } fastPaths;

        uint32_t displayListAddress;
        uint64_t displayListCounter;
        std::vector<Projection> projections;
        uint32_t projectionCount;
        uint32_t projectionStart;
        uint32_t gameCallCount;
        bool depthRead;
        bool depthWrite;
        bool syncRequired;
        bool fillRectOnly;
        std::vector<RDPAddress> startFbDiscards;
        std::vector<FramebufferOperation> startFbOperations;
        std::vector<FramebufferOperation> endFbOperations;
        std::array<uint32_t, 4> ditherPatterns;
        FixedRect scissorRect;
        FixedRect drawColorRect;
        FixedRect drawDepthRect;
        FlushReason flushReason;

        void reset();
        void addGameCall(const GameCall &gameCall);
        bool inProjection(uint32_t transformsIndex, Projection::Type type) const;
        int changeProjection(uint32_t transformsIndex, Projection::Type type);
        bool isEmpty() const;
        bool earlyPresentCandidate() const;
    };
};