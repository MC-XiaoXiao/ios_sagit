// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../../security/QcomSCM/SCMPAS.h"
#include "OSMTZContract.h"
namespace QcomOSMInit {
// SCM atomic IO ABI: pinned qcom_scm.c:840-873. The caller separately checks
// the fixed OSM register allowlist; this transport also rejects addresses that
// stock TZ would silently ignore. A failed/ambiguous call is never retried as
// a new transaction, and an unfinished continuation poisons this transport.
template <class Backend> class SecureIO {
    Backend &backend;
    QcomSecure::Transport<Backend> probeTransport;
    bool ready = false, poisoned = false;
    bool invoke(unsigned command, uint32_t address, uint32_t &value) {
        if (!ready || poisoned || !QcomOSMTZ::permitted(address)) return false;
        const QcomSecure::Registers initial{{
            QcomSecure::sip(5, command) | 0x80000000U |
                (probeTransport.is64() ? 0x40000000U : 0),
            command == 1 ? 1U : 2U, address, command == 2 ? value : 0U,
            0, 0, 0, 0}};
        uint64_t function = initial.x[0], continuation = 0;
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            auto r = initial;
            r.x[0] = function; r.x[6] = continuation;
            backend.smc(r);
            if (int32_t(r.x[0]) == 1) {
                function = 1; continuation = r.x[6]; continue;
            }
            if (int32_t(r.x[0]) != 0) { poisoned = true; return false; }
            if (command == 1) value = uint32_t(r.x[1]);
            return true;
        }
        poisoned = true; return false;
    }
public:
    explicit SecureIO(Backend &io) : backend(io), probeTransport(io) {}
    bool probe() {
        if (poisoned) return false;
        if (ready) return true;
        if (probeTransport.probe() != QcomSecure::Status::OK) return false;
        const auto read = probeTransport.available(5, 1);
        if (read.status != QcomSecure::Status::OK || read.value != 1) return false;
        const auto write = probeTransport.available(5, 2);
        ready = write.status == QcomSecure::Status::OK && write.value == 1;
        return ready;
    }
    bool read(uint32_t address, uint32_t &value) { return invoke(1, address, value); }
    bool write(uint32_t address, uint32_t value) { return invoke(2, address, value); }
};
}
