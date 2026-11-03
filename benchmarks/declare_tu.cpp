// One plain consumer translation unit: includes the device header selected by
// DEVICE_HEADER and touches register handles. No layout is defined here.
#include DEVICE_HEADER
#ifndef INCLUDE_ONLY
int consume(declare_device::Device& device) {
    int sink = 0;
    for (unsigned v = 0; v < 9; ++v)
        device.vf[v].A0.at(0).ref() = v;
    sink += static_cast<int>(device.vf[0].A1.size());
    return sink;
}
#endif
