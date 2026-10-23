// SPDX-License-Identifier: BSD-2-Clause
#pragma once

namespace MetalLifetime {
// A borrowed reference must zero on destruction: a recycled native resource
// ID cannot make an expired encoding refer to an unrelated new allocation.
// Loading it holds an ARC temporary only for the current CPU operation.
template<class Object> class Reference {
    Object __strong _retained = nullptr;
    Object __weak _borrowed = nullptr;
    bool _assigned = false;
public:
    void set(Object object, bool retain) {
        _assigned = object != nullptr;
        // Store the new weak reference before releasing a previous owner.
        _borrowed = retain ? nullptr : object;
        _retained = retain ? object : nullptr;
    }
    Object get() const { return _retained ?: _borrowed; }
    bool assigned() const { return _assigned; }
    bool expired() const { return _assigned && !get(); }
    void borrow() { bool assigned=_assigned;Object object=get();set(object,false);_assigned=assigned; }
};
}
