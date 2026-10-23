#include "QUPI2C.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <map>

struct Registers {
    std::map<unsigned,unsigned> values;
    std::vector<uint8_t> tx,rx;
    unsigned received=0,resets=0,writes=0;
    uint64_t time=0;
    bool stall=false,busy=false,badState=false,nack=false,error=false,master=true,fifoDoneLag=false;
    uint32_t read32(uint32_t reg) {
        using namespace QUPI2C;
        if (reg==State) return badState ? 0 : 4|(master ? 16 : 0)|(values[State]&3);
        if (reg==Status) return busy ? 0x100 : nack && !tx.empty() ? 8 : 0;
        if (reg==Errors) return error && !tx.empty() ? 4 : 0;
        if (reg==Operational) {
            if (stall) return 0;
            unsigned size=values[OutputCount]+values[WriteCount];
            if (tx.size()<size) return 0x1100;
            unsigned op=fifoDoneLag ? 0x100 : 0x400;
            if (received<rx.size()) op|=0x2200;
            else op|=0x800;
            op&=~(values[OpMask]&0x300); // real hardware suppresses service flags
            return op;
        }
        if (reg==InFIFO) {
            uint32_t word=0;
            for (unsigned i=0;i<4 && received<rx.size();++i) word|=uint32_t(rx[received++])<<(i*8);
            return word;
        }
        return values[reg];
    }
    void write32(uint32_t reg,uint32_t value) {
        ++writes;
        if (reg==QUPI2C::Reset) { ++resets;values[QUPI2C::State]=0;return; }
        if (reg==QUPI2C::OutFIFO) {
            unsigned size=values[QUPI2C::OutputCount]+values[QUPI2C::WriteCount];
            for (unsigned i=0;i<4 && tx.size()<size;++i) tx.push_back(value>>(8*i));
            return;
        }
        if (reg==QUPI2C::Errors || reg==QUPI2C::Status) return;
        values[reg]=value;
    }
    uint64_t nowUS() { return time; }
    void delayUS(unsigned us) { time+=us; }
};
int main() {
    using namespace I2C;
    uint8_t index=0x81,output[80]{};
    Message messages[]={{Direction::Write,&index,1},{Direction::Read,output,sizeof(output)}};
    Transaction t{0x20,messages,2,20000};
    QUPI2C::Packet p;
    assert(QUPI2C::encode(t,p));
    const uint8_t expected[]={0x81,0x40,0x82,1,0x81,0x81,0x41,0x87,80};
    assert(p.txSize==sizeof(expected) && p.rxSize==82);
    for (unsigned i=0;i<sizeof(expected);++i) assert(p.tx[i]==expected[i]);
    Registers io;
    io.rx={0x87,80};
    for (unsigned i=0;i<80;++i) io.rx.push_back(i+5);
    QUPI2C::Engine<Registers> engine(io);
    assert(engine.configure(19200000,400000));
    assert(engine.transfer(t)==Result::Success);
    assert(io.tx==std::vector<uint8_t>(expected,expected+sizeof(expected)));
    assert(io.received==82 && io.resets==2);
    for (unsigned i=0;i<80;++i) assert(output[i]==i+5);
    assert(io.values[QUPI2C::InputCount]==82); // real block RX, final partial block
    assert(io.values[QUPI2C::Clock]==0xe031c);
    // FIFO RX excludes two transport bytes and handles a partial 32-bit word.
    messages[1].length=3;
    Registers small;small.rx={0x87,3,0x11,0x22,0x33};
    QUPI2C::Engine<Registers> fifo(small);assert(fifo.configure(19200000,100000));
    assert(fifo.transfer(t)==Result::Success);
    assert(output[0]==0x11 && output[1]==0x22 && output[2]==0x33);
    // Hardware can assert FIFO service without MAX_OUTPUT_DONE (Linux quirk).
    Registers delayed;delayed.values[QUPI2C::Mode]=4;delayed.fifoDoneLag=true;delayed.rx={0x87,3,4,5,6};
    QUPI2C::Engine<Registers> delayedEngine(delayed);assert(delayedEngine.configure(19200000,400000));
    assert(delayedEngine.transfer(t)==Result::Success && output[0]==4 && output[2]==6);
    // A full 256-byte write exercises block TX and the zero length tag.
    uint8_t payload[256];for (unsigned i=0;i<256;++i) payload[i]=i;
    Message write{Direction::Write,payload,256};
    Transaction longWrite{0x34,&write,1,20000};
    Registers longIO;QUPI2C::Engine<Registers> longEngine(longIO);
    assert(longEngine.configure(19200000,400000));
    assert(longEngine.transfer(longWrite)==Result::Success);
    assert(longIO.tx.size()==260 && longIO.tx[2]==0x83 && longIO.tx[3]==0);
    for (unsigned i=0;i<256;++i) assert(longIO.tx[4+i]==payload[i]);
    // Multiple reads require NACK/repeated START, one final STOP.
    Message reads[]={{Direction::Read,output,2},{Direction::Read,output+2,3}};
    Transaction multi{0x20,reads,2};
    assert(QUPI2C::encode(multi,p) && p.tx[2]==0x86 && p.tx[6]==0x87 && p.rxSize==9);
    Registers multiIO;multiIO.rx={0x86,2,9,8,0x87,3,7,6,5};
    QUPI2C::Engine<Registers> multiple(multiIO);assert(multiple.configure(19200000,400000));
    assert(multiple.transfer(multi)==Result::Success);
    for (unsigned i=0;i<5;++i) assert(output[i]==9-i);
    for (unsigned block=0;block<3;++block) {
        Registers sized;sized.values[QUPI2C::Mode]=block|(block<<5);
        sized.rx={0x87,3,1,2,3};
        QUPI2C::Engine<Registers> sizedEngine(sized);
        assert(sizedEngine.configure(19200000,400000) && sizedEngine.transfer(t)==Result::Success);
    }
    Registers reserved;reserved.values[QUPI2C::Mode]=3;
    QUPI2C::Engine<Registers> reservedEngine(reserved);assert(!reservedEngine.configure(19200000,400000));
    for (unsigned fault=0;fault<5;++fault) {
        Registers broken;broken.rx={0x87,3,1,2,3};
        broken.nack=fault==0;broken.error=fault==1;broken.stall=fault==2;
        broken.badState=fault==3;broken.busy=fault==4;
        QUPI2C::Engine<Registers> hw(broken);assert(hw.configure(19200000,400000));
        output[0]=0xab;
        auto status=hw.transfer(t);
        assert(status==(fault==0 ? Result::Nack : fault==1 ? Result::BusError : Result::Timeout));
        assert(output[0]==0xab); // no partial success returned to client
        assert(broken.time<=t.timeoutUS+10);
        assert(fault==4 ? broken.resets==0 : broken.resets==2);
    }
    // Argument rejection performs no MMIO.
    Registers invalid;QUPI2C::Engine<Registers> bad(invalid);assert(bad.configure(19200000,400000));
    t.address=0x80;assert(bad.transfer(t)==Result::Invalid && invalid.writes==0);
    t.address=0x20;messages[0].length=0;assert(!valid(t));
    messages[0].length=257;assert(!valid(t));
    messages[0].length=1;t.timeoutUS=0;assert(!valid(t));
    t.timeoutUS=20000;t.count=9;assert(!valid(t));
    uint32_t clock;
    assert(!QUPI2C::clockControl(19200000,0,clock));
    assert(QUPI2C::clockControl(50000000,100000,clock)); // divider remains representable
    assert(!QUPI2C::clockControl(1000000,400000,clock));
    puts("i2c: combined transfers, FIFO/block tails, errors and deadlines passed");
}
