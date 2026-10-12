// Stub. See firmware/test/stubs/README.md.
#pragma once
#include "../configuration.h"

namespace concurrency {
class OSThread
{
  public:
    explicit OSThread(const char *name) : threadName(name) {}
    virtual ~OSThread() {}
  protected:
    virtual int32_t runOnce() { return 1000; }
    const char *threadName;
};
} // namespace concurrency
