#pragma once
#if defined(FLOWE_ALLOCATION_PROBE)
void allocationProbeStart();
void allocationProbePhase(unsigned phase);
void allocationProbeFinish();
void allocationProbeSelfTest();
#else
inline void allocationProbeStart() {}
inline void allocationProbePhase(unsigned) {}
inline void allocationProbeFinish() {}
inline void allocationProbeSelfTest() {}
#endif
