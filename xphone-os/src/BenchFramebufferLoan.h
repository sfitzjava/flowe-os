#pragma once
// The one-boot BSS donation experiment is retired. A heap region cannot be
// registered twice. Sync now uses FreeInkDisplay's explicit ownership API.
#if defined(FLOWE_BENCH_FRAMEBUFFER_LOAN)
#error "FLOWE_BENCH_FRAMEBUFFER_LOAN is retired; use automatic sync framebuffer release"
#endif
