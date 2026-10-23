// SPDX-License-Identifier: BSD-2-Clause
#include "WLANFirmwareClient.h"
#include "WLANQRTR.h"
#include "WLANTarget.h"
OSDefineMetaClassAndAbstractStructors(QcomWLANFirmwareEndpoint,OSObject);
namespace WLAN {
struct FirmwareClient::State {
    QcomWLANFirmwareEndpoint &endpoint;
    QRTR::Session<QcomWLANFirmwareEndpoint> session;
    WLFW<decltype(session)> firmware;
    State(QcomWLANFirmwareEndpoint &io,uint32_t node) : endpoint(io),session(io,node),firmware(session) {
        endpoint.retain();
    }
    ~State() { endpoint.release(); }
};
FirmwareClient::FirmwareClient(State *storage) : state(storage) {}
FirmwareClient *FirmwareClient::create(QcomWLANFirmwareEndpoint *endpoint,uint32_t node) {
    if (!endpoint || !node || node == QRTR::BroadcastNode) return nullptr;
    auto *storage = new State(*endpoint,node);
    if (!storage) return nullptr;
    auto *client = new FirmwareClient(storage);
    if (!client) delete storage;
    return client;
}
Result FirmwareClient::bootstrap(const FirmwareConfiguration &configuration,uint64_t epoch) {
    if (!configuration.valid()) return Result::Invalid;
    auto result = state->session.connect(epoch);
    if (result != Result::Success) return result;
    return state->firmware.bootstrap(configuration,epoch);
}
Result FirmwareClient::enable() {
    uint8_t payload[1024]; size_t bytes = 0;
    auto result = targetConfiguration(payload,sizeof(payload),bytes);
    return result == Result::Success ? state->firmware.configure(payload,bytes) : result;
}
Result FirmwareClient::shutdown() {
    const auto result = state->firmware.shutdown();
    if (result != Result::Success) return result;
    return state->session.close();
}
bool FirmwareClient::ownsPermissions() const { return state->firmware.ownsPermissions(); }
bool FirmwareClient::ownsResources() const {
    return state->firmware.ownsSession() || state->firmware.ownsPermissions() || state->session.hasPort();
}
Result FirmwareClient::destroy() {
    const auto result = shutdown();
    if (result != Result::Success) return result;
    delete state; state = nullptr; delete this;
    return Result::Success;
}
}
