// Stub. See firmware/test/stubs/README.md.
#pragma once
#include "mesh-types.h"

class NodeDB
{
  public:
    NodeNum getNodeNum();
};
extern NodeDB *nodeDB;
