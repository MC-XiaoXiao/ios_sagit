// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <libkern/c++/OSObject.h>
#include "WLANWLFW.h"

// A retained in-kernel GLINK endpoint. Its implementation must supply actual
// datagrams, remote restart invalidation and SCM permission transactions.
// unshareMSA must verify CPU ownership even after a partial/failed share.
// RPM's intentless endpoint does not implement this contract.
class QcomWLANFirmwareEndpoint : public OSObject {
    OSDeclareAbstractStructors(QcomWLANFirmwareEndpoint);
public:
    // The channel owner allocates monotonic, nonreused local ports across
    // client objects. A new epoch alone cannot disambiguate an old QMI ID.
    virtual WLAN::Result allocatePort(uint32_t,uint64_t,uint32_t &) = 0;
    virtual uint64_t now() = 0; // monotonic milliseconds
    virtual WLAN::Result send(const void *,size_t,uint64_t) = 0;
    virtual WLAN::Result receive(void *,size_t,size_t &,uint64_t) = 0;
    virtual WLAN::Result shareMSA(uint64_t,const WLAN::MSARegion *,unsigned) = 0;
    virtual WLAN::Result unshareMSA(uint64_t,const WLAN::MSARegion *,unsigned) = 0;
};
namespace WLAN {
class FirmwareClient {
    struct State;
    State *state;
    explicit FirmwareClient(State *);
    ~FirmwareClient() = default;
public:
    static FirmwareClient *create(QcomWLANFirmwareEndpoint *,uint32_t);
    FirmwareClient(const FirmwareClient &) = delete;
    FirmwareClient &operator=(const FirmwareClient &) = delete;
    Result bootstrap(const FirmwareConfiguration &,uint64_t);
    Result enable();
    Result shutdown();
    bool ownsPermissions() const;
    bool ownsResources() const;
    Result destroy();
};
}
