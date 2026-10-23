// SPDX-License-Identifier: BSD-2-Clause
#pragma once
namespace Display {
// Only completed command transfers enter this two-owner idle list. Taking a
// mapping transfers its reference back to staged/prepared ownership. Active,
// front and prepared objects are never simultaneously owned by this list.
template<class Mapping> class IdleMappings {
    Mapping *slots[2]{};
public:
    template<class Surface> Mapping *take(Surface *surface) {
        for(auto &slot:slots)if(slot && slot->surface()==surface) {
            auto *mapping=slot;slot=nullptr;return mapping;
        }
        return nullptr;
    }
    Mapping *put(Mapping *mapping) {
        for(auto &slot:slots)if(!slot){slot=mapping;return nullptr;}
        auto *old=slots[0];slots[0]=slots[1];slots[1]=mapping;return old;
    }
    Mapping *pop() {
        for(auto &slot:slots)if(slot){auto *mapping=slot;slot=nullptr;return mapping;}
        return nullptr;
    }
    unsigned count() const {return unsigned(slots[0]!=nullptr)+unsigned(slots[1]!=nullptr);}
};
}
