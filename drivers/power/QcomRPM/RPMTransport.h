// SPDX-License-Identifier: GPL-2.0-only
// GLINK RPM TOC and wire protocol: Linux v6.12 qcom_glink_{rpm,native}, smd-rpm.
#pragma once
#include <QcomPowerHardware.h>
namespace QcomPower {
// The boot owner verified the installed GLINK RPM transport and observed all
// four cursors at zero after EFI exit. No channel or resource ACK is implied.
// Local TX head/RX tail remain zero until this sole consumer starts. Fresh
// remote VERSION traffic may arrive meanwhile; it must never be discarded.
struct RPMTransportCold {
    uint32_t version=1,bytes=sizeof(RPMTransportCold),flags=0,ramBytes=0;
    uint32_t txOffset=0,txLength=0,rxOffset=0,rxLength=0,nextMessageID=0,reserved=0;
    enum : uint32_t { ProducerStopped=1, ResetFenced=2, EFIExited=4, Required=7 };
    bool valid() const {
        return version==1 && bytes==sizeof(*this) && flags==Required && ramBytes==0x7000 && nextMessageID && !reserved &&
            txLength>=512 && rxLength>=512 && txLength<=0x6ef8 && rxLength<=0x6ef8 &&
            txOffset<=0x6ef8-txLength && rxOffset<=0x6ef8-rxLength &&
            !((txOffset|txLength|rxOffset|rxLength)&7) &&
            (txOffset+txLength+8<=rxOffset || rxOffset+rxLength+8<=txOffset);
    }
};
static_assert(sizeof(RPMTransportCold)==40,"RPM cold transport handoff ABI");
struct RPMTransportHandoff {
    uint32_t version, flags, txOffset, txLength, rxOffset, rxLength;
    uint32_t txHead, rxTail, messageID, remoteID, reserved[2];
    enum : uint32_t { Connected=1, Drained=2, ProducerStopped=4, Required=7 };
    bool valid() const {
        return version==1 && flags==Required && messageID && remoteID && remoteID<=65535 &&
            !reserved[0] && !reserved[1] && txLength>=512 && rxLength>=512 &&
            !((txOffset|txLength|rxOffset|rxLength|txHead|rxTail)&7) && txHead<txLength && rxTail<rxLength;
    }
};
static_assert(sizeof(RPMTransportHandoff)==48,"RPM channel handoff ABI");
class RPMTransport : public Registers {
    struct Pipe { uint32_t offset = 0, length = 0; } rx, tx;
    uint32_t messageID = 0, expectedID = 0;
    uint16_t remoteID = 0;
    bool version = false, localAck = false, remoteOpen = false, fault = false, mapped = false, yielded = false;
    uint8_t reply[256] = {};
    uint32_t replyBytes = 0, replyTotal = 0;
    uint32_t lastHeader = 0, lastParam = 0, lastChunk = 0, lastLeft = 0, lastReplyBytes = 0;
    uint32_t openAckWord = 0;
    bool openAckObserved = false;
    uint32_t lastType = 0, lastResource = 0, lastSet = 0;
    uint16_t discovered[16]{};
    Status acknowledgement = NotReady;
    bool cursor(const Pipe &p, uint32_t &head, uint32_t &tail) const {
        head = rd(p.offset + 4); tail = rd(p.offset);
        return head < p.length && tail < p.length && !(head & 7) && !(tail & 7);
    }
    uint32_t available(const Pipe &p, uint32_t head, uint32_t tail) const {
        return (head + p.length - tail) % p.length;
    }
    void peek(uint8_t *out, uint32_t count, uint32_t skip = 0) const {
        uint32_t tail = rd(rx.offset);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t index = (tail + skip + i) % rx.length;
            out[i] = static_cast<uint8_t>(rd(rx.offset + 8 + (index & ~3U)) >> (8*(index & 3)));
        }
    }
    void consume(uint32_t count) { wr(rx.offset, (rd(rx.offset) + count) % rx.length); }
    Status send(uint16_t command, uint16_t channel, uint32_t param,
        const uint8_t *payload = nullptr, uint32_t count = 0) {
        uint32_t size = align8(8 + count), head, tail;
        if (count > 272 || !cursor(tx, head, tail)) return Invalid;
        if (tx.length - available(tx, head, tail) <= size + 8) return Busy;
        uint8_t frame[280] = {};
        put32(frame, uint32_t(command) | uint32_t(channel) << 16); put32(frame+4, param);
        for (unsigned i = 0; i < count; ++i) frame[8+i] = payload[i];
        for (uint32_t i = 0; i < size; i += 4) wr(tx.offset+8+(head+i)%tx.length, le32(frame+i));
        wr(tx.offset+4, (head+size)%tx.length); io.kick(io.context); return OK;
    }
    Status parseReply() {
        if (replyBytes < 8 || le32(reply) != 0x00716572 || le32(reply+4) != replyBytes-8) return Invalid;
        bool found = false, error = false; uint32_t id = 0;
        for (uint32_t pos = 8; pos < replyBytes;) {
            if (replyBytes-pos < 8) return Invalid;
            uint32_t type = le32(reply+pos), length = le32(reply+pos+4);
            if (length > replyBytes-pos-8 || length > 248) return Invalid;
            if (type == 0x2367736d) {
                if (found || length != 4) return Invalid;
                found = true; id = le32(reply+pos+8);
            } else if (type == 0x00727265) error = true;
            uint32_t step = 8 + ((length+3)&~3U);
            if (step > replyBytes-pos) return Invalid;
            pos += step;
        }
        if (!found) return Invalid;
        if (id != expectedID || !expectedID) return NotReady; // stale ACK never completes a new vote.
        return error ? Denied : OK;
    }
    Status step() {
        uint32_t head, tail;
        if (!cursor(rx, head, tail)) return Invalid;
        uint32_t avail = available(rx, head, tail);
        if (avail < 8) return NotReady;
        uint8_t header[16] = {}; peek(header, 8);
        uint16_t command = le32(header) & 0xffff, cid = le32(header) >> 16;
        uint32_t param = le32(header+4), size = 8;
        lastHeader = le32(header); lastParam = param;
        Status result = OK;
        switch (command) {
        case 0: // version, RPM is an intentless transport even with feature bits 0.
            if (version && localAck && remoteOpen) return Stale;
            if (cid != 1) return Unsupported;
            result = send(1, 1, 0); break;
        case 1:
            if(version && localAck && remoteOpen)return Stale;
            if (cid != 1 || param) return Unsupported;
            version = true; break;
        case 2: {
            if (!cid || !param || param > 64) return Invalid;
            size = align8(8+param);
            if (avail < size) return NotReady;
            uint8_t name[64] = {}; peek(name, param, 8);
            if (name[param-1]) return Invalid;
            for (unsigned i=0;i+1<param;++i) if (!name[i]) return Invalid;
            const char expected[] = "rpm_requests";
            bool matched=param==sizeof(expected);
            for (unsigned i=0;matched && i<sizeof(expected);++i) matched=name[i]==uint8_t(expected[i]);
            if (!matched) {
                // RPM opens other logical services on this same link. Do not
                // acknowledge/bind those channels or mistake their discovery
                // for corruption of the owned rpm_requests connection.
                if (remoteID==cid) return Stale;
                bool remembered=false;
                for (auto id:discovered) remembered |= id==cid;
                if (!remembered) {
                    for (auto &id:discovered) if (!id) { id=cid; remembered=true; break; }
                    if (!remembered) return Unsupported;
                }
                break;
            }
            if (remoteID && remoteID != cid) return Stale;
            result = send(4, cid, 0);
            if (result == OK) { remoteID = cid; remoteOpen = true; }
            break;
        }
        case 4:
            // OPEN_ACK identifies our local channel. Its trailing word is
            // not interpreted by the native GLINK RX protocol (Linux v6.12
            // qcom_glink_rx_open_ack). The RPM peer can return a nonzero word;
            // it must not be confused with an RPM resource acknowledgement.
            if (cid != 1) return Invalid;
            openAckWord = param; openAckObserved = true;
            localAck = true; break;
        case 9:
        case 12: {
            if (cid != remoteID || !remoteOpen) return Invalid;
            if (avail < 16) return NotReady;
            peek(header, 16);
            uint32_t chunk = le32(header+8), left = le32(header+12);
            lastChunk = chunk; lastLeft = left;
            if (!chunk || chunk > 256 || left > 256-chunk) return Invalid;
            size = align8(16+chunk);
            if (avail < size) return NotReady;
            if (command == 9) {
                if (replyBytes) return Invalid;
                replyTotal = chunk+left;
            } else if (!replyBytes || replyBytes+chunk+left != replyTotal) return Invalid;
            if (replyBytes+chunk > sizeof(reply) || replyBytes+chunk+left != replyTotal) return Invalid;
            peek(reply+replyBytes, chunk, 16); replyBytes += chunk;
            if (!left) {
                lastReplyBytes = replyBytes;
                result = parseReply(); replyBytes = replyTotal = 0;
                if (result == OK || result == Denied) acknowledgement = result;
                if (result == NotReady) result = OK;
            }
            break;
        }
        case 13: io.kick(io.context); break;
        case 15: if (cid != remoteID) return Invalid; break;
        case 3: {
            if (!cid) return Invalid;
            if (cid==remoteID) return Stale;
            uint16_t *known=nullptr;
            for (auto &id:discovered) if (id==cid) known=&id;
            if (!known) return Stale;
            result=send(11,cid,0);
            if (result==OK) *known=0;
            break;
        }
        case 11: return Stale; // No local channel other than1 is allocated.
        default: return Unsupported;
        }
        if (result == Busy) return Busy; // retain frame until its response can be queued.
        if (result != OK && result != Denied) return result;
        consume(size); return OK;
    }
    Status locate(IO access, uint32_t ramSize) {
        setIO(access);
        if (mapped || !io.read || !io.write || !io.wait || !io.kick || ramSize < 512 || (ramSize & 7)) return Invalid;
        uint32_t toc = ramSize-256;
        if (rd(toc) != 0x67727430 || rd(toc+4) > 20) return Invalid;
        for (unsigned i = 0; i < rd(toc+4); ++i) {
            uint32_t id = rd(toc+8+12*i), offset = rd(toc+12+12*i), length = rd(toc+16+12*i);
            if (id != 0x61703272 && id != 0x72326170) continue;
            if ((offset & 7) || (length & 7) || length < 512 || length > toc-8 || offset > toc-8-length) return Invalid;
            Pipe &pipe = id == 0x61703272 ? tx : rx;
            if (pipe.length) return Invalid;
            pipe = {offset, length};
        }
        if (!rx.length || !tx.length || !(rx.offset+rx.length+8 <= tx.offset || tx.offset+tx.length+8 <= rx.offset)) return Invalid;
        return OK;
    }
    bool coldInput(uint32_t head) const {
        if (head>256) return false; // at most 32 initial negotiation frames
        for (uint32_t offset=0;offset<head;offset+=8) {
            uint8_t header[8]{};peek(header,8,offset);
            const auto word=le32(header),command=word&65535;
            if ((word>>16)!=1 || (command!=0 && command!=1) || (command==1 && le32(header+4))) return false;
        }
        return true;
    }
public:
    // Read-only bring-up diagnostics. Captures framing/resource identities;
    // never resets a FIFO, consumes a frame or changes a resource lease.
    struct Debug {
        uint32_t state, rxOffset, rxLength, rxTail, rxHead, txOffset, txLength, txTail, txHead;
        uint32_t message, expected, remote, ack, header, param, chunk, left, replyBytes;
        uint32_t type, resource, set;
        uint32_t reply[16];
    };
    Debug debug() const {
        Debug d{};
        d.state=mapped | uint32_t(version)<<1 | uint32_t(localAck)<<2 | uint32_t(remoteOpen)<<3 |
            uint32_t(fault)<<4 | uint32_t(yielded)<<5;
        d.rxOffset=rx.offset; d.rxLength=rx.length; d.txOffset=tx.offset; d.txLength=tx.length;
        if (io.read && rx.length) { d.rxTail=rd(rx.offset); d.rxHead=rd(rx.offset+4); }
        if (io.read && tx.length) { d.txTail=rd(tx.offset); d.txHead=rd(tx.offset+4); }
        d.message=messageID; d.expected=expectedID; d.remote=remoteID; d.ack=acknowledgement;
        d.header=lastHeader; d.param=lastParam; d.chunk=lastChunk; d.left=lastLeft; d.replyBytes=lastReplyBytes;
        d.type=lastType; d.resource=lastResource; d.set=lastSet;
        for (unsigned i=0;i<16;++i) if (4*i<lastReplyBytes) d.reply[i]=le32(reply+4*i);
        return d;
    }
    // A native consumer adopting a boot channel has not observed this frame.
    bool observedOpenAck(uint32_t &word) const {
        if (!openAckObserved) return false;
        word = openAckWord; return true;
    }
    Status initialize(IO access, uint32_t ramSize) {
        const auto status=locate(access,ramSize); if(status!=OK)return status;
        uint32_t th,tt,rh,rt;
        if (!cursor(tx,th,tt) || !cursor(rx,rh,rt) || th || tt || rh || rt) return Stale;
        mapped = true;
        // Caller owns the cold HLOS transport. Dirty cursors are evidence of
        // another epoch/producer, never permission to reset its FIFO state.
        return send(0, 1, 0);
    }
    Status initializeCold(IO access,uint32_t ramSize,const RPMTransportCold &h) {
        if (!h.valid() || h.ramBytes!=ramSize) return Invalid;
        const auto status=locate(access,ramSize);if(status!=OK)return status;
        uint32_t th,tt,rh,rt;
        if (tx.offset!=h.txOffset || tx.length!=h.txLength || rx.offset!=h.rxOffset || rx.length!=h.rxLength ||
            !cursor(tx,th,tt) || !cursor(rx,rh,rt) || th || tt || rt || !coldInput(rh)) return Stale;
        messageID=h.nextMessageID-1;mapped=true;
        return send(0,1,0); // fresh handshake; no cursor reset and no inferred ACK
    }
    bool ready() const { return mapped && version && localAck && remoteOpen && !fault && !yielded; }
    Status adopt(IO access,uint32_t ramSize,const RPMTransportHandoff &h) {
        if(!h.valid() || mapped || yielded)return Invalid;
        const auto status=locate(access,ramSize);if(status!=OK)return status;
        uint32_t th,tt,rh,rt;
        if(tx.offset!=h.txOffset || tx.length!=h.txLength || rx.offset!=h.rxOffset || rx.length!=h.rxLength ||
            !cursor(tx,th,tt) || !cursor(rx,rh,rt) || th!=h.txHead || tt!=th || rt!=h.rxTail)return Stale;
        // Remote RX head may advance after publication. Local cursors and the
        // drained TX tail must still belong to the stopped bridge producer.
        messageID=h.messageID;remoteID=uint16_t(h.remoteID);
        version=localAck=remoteOpen=mapped=true;
        return check(); // consume pending reset/close before admitting requests
    }
    Status handoff(RPMTransportHandoff &h,unsigned timeoutUsec=100000) {
        h={};if(!ready() || expectedID || replyBytes || !messageID)return NotReady;
        for(unsigned elapsed=0;elapsed<=timeoutUsec;elapsed+=100) {
            const auto status=check();if(status!=OK && status!=Busy)return status;
            uint32_t th,tt,rh,rt;
            if(!cursor(tx,th,tt) || !cursor(rx,rh,rt))return Invalid;
            if(status==OK && th==tt && rh==rt && !replyBytes) {
                h={1,RPMTransportHandoff::Required,tx.offset,tx.length,rx.offset,rx.length,th,rt,messageID,remoteID,{0,0}};
                yielded=true;return OK; // this instance can never publish again
            }
            if(elapsed!=timeoutUsec)wait(100);
        }
        fault=true;return TimedOut;
    }
    Status check() {
        if (!ready()) return NotReady;
        // Consume pending remote close/reset and keepalive frames even when
        // the caller only checks an existing bandwidth grant. Bound the work
        // so a remote flood cannot monopolize the common power workloop.
        for (unsigned i=0;i<32;++i) {
            const Status s=step();
            if (s==NotReady) return OK;
            if (s==Busy) return Busy;
            if (s!=OK) { fault=true; return Stale; }
        }
        return Busy;
    }
    Status connect(unsigned timeoutUsec = 500000) {
        if (!mapped || fault || yielded) return NotReady;
        bool openSent = false;
        for (unsigned elapsed = 0; elapsed <= timeoutUsec; elapsed += 100) {
            if (ready()) return OK;
            if (version && !openSent) {
                const uint8_t name[] = "rpm_requests";
                Status s = send(2, 1, sizeof(name), name, sizeof(name));
                if (s == OK) openSent = true;
                else if (s != Busy) { fault = true; return s; }
            }
            Status s = step();
            if (s != OK && s != NotReady && s != Busy) { fault = true; return s; }
            if (ready()) return OK;
            if (elapsed != timeoutUsec) wait(100);
        }
        fault = true; return TimedOut;
    }
    Status write(uint32_t type, uint32_t id, uint32_t set, const uint8_t *kvp, uint32_t bytes,
        unsigned timeoutUsec = 500000) {
        if (!ready()) return NotReady;
        if (set > 1 || !kvp || !bytes || bytes > 216) return Invalid;
        // Regulator/clock values are u32; MSM8998 bus bandwidth is u64.
        // Validate each complete KVP before publishing any FIFO bytes.
        for (uint32_t pos=0;pos<bytes;) {
            if (bytes-pos<8) return Invalid;
            const auto length=le32(kvp+pos+4);
            if ((length!=4 && length!=8) || length>bytes-pos-8) return Invalid;
            pos+=8+length;
        }
        uint8_t packet[248] = {};
        lastType=type; lastResource=id; lastSet=set;
        if (++messageID == 0) ++messageID;
        expectedID = messageID; acknowledgement = NotReady;
        put32(packet, 0x00716572); put32(packet+4, 20+bytes);
        put32(packet+8, expectedID); put32(packet+12, set); put32(packet+16, type);
        put32(packet+20, id); put32(packet+24, bytes);
        for (unsigned i = 0; i < bytes; ++i) packet[28+i] = kvp[i];
        uint8_t frame[256] = {}; put32(frame, 28+bytes);
        for (unsigned i = 0; i < 28+bytes; ++i) frame[8+i] = packet[i];
        Status s = send(9, 1, 0, frame, 36+bytes);
        if (s != OK) { expectedID = 0; return s; }
        for (unsigned elapsed = 0; elapsed <= timeoutUsec; elapsed += 100) {
            if (acknowledgement != NotReady) { expectedID = 0; return acknowledgement; }
            s = step();
            if (s != OK && s != NotReady && s != Busy) {
                fault = true; expectedID = 0;
                // The request is already visible to RPM. A malformed reply
                // does not prove that a lowering vote was rejected.
                return Stale;
            }
            if (acknowledgement != NotReady) { expectedID = 0; return acknowledgement; }
            if (elapsed != timeoutUsec) wait(100);
        }
        expectedID = 0; fault = true; return TimedOut;
    }
};
}
