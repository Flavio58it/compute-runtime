/*
 * Copyright (C) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/os_interface/windows/wddm_allocation.h"

namespace OCLRT {
std::string WddmAllocation::getAllocationInfoString() const {
    std::stringstream ss;
    ss << " Handle: " << handle;
    return ss.str();
}
} // namespace OCLRT