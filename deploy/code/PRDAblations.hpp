#pragma once

#include "PRD.hpp"

// Paper-only variants. The production policy is PRD in PRD.hpp.
class PRDAdaptiveGhost final : public PRDCore {
 public:
  PRDAdaptiveGhost(std::size_t k, std::size_t c, int d, double r)
      : PRDCore(k, c, d, r, 1, 0, false, false, true, true, 0, 8.0,
                true, false, 8.0) { EnableAdaptiveGhost(); }
};

class PRDAdaptiveLease final : public PRDCore {
 public:
  PRDAdaptiveLease(std::size_t k, std::size_t c, int d, double r)
      : PRDCore(k, c, d, r, 1, 0, false, false, true, true, 0, 8.0,
                true, false, 8.0) { EnableAdaptiveLease(); }
};

class PRDNullDepth final : public PRDCore {
 public:
  PRDNullDepth(std::size_t k, std::size_t c, int d, double r)
      : PRDCore(k, c, d, r, 1, 0, true, false, true, true, 0, 8.0,
                true, false, 8.0) {
    EnableAdaptiveGhost(); EnableAdaptiveLease();
  }
};

class PRDPureDepth final : public PRDCore {
 public:
  PRDPureDepth(std::size_t k, std::size_t c, int d, double r)
      : PRDCore(k, c, d, r, 1, 0, false, false, true, true, 0, 8.0,
                false, true, 8.0) {
    EnableAdaptiveGhost(); EnableAdaptiveLease();
  }
};

class PRDColdFifo final : public PRDCore {
 public:
  PRDColdFifo(std::size_t k, std::size_t c, int d, double r)
      : PRDCore(k, c, d, r, 1, 0, false, false, true, true, 0, 8.0,
                false, false, 8.0, true, true) {
    EnableAdaptiveGhost(); EnableAdaptiveLease();
  }
};
