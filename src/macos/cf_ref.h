// SPDX-License-Identifier: MIT
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <utility>

namespace kcf::macos {

// Owning reference to a Core Foundation object (released with CFRelease).
template <typename T>
class CFRef {
public:
    CFRef() noexcept = default;
    explicit CFRef(T ref) noexcept : ref_(ref) {}  // adopts a +1 reference
    ~CFRef() { reset(); }
    CFRef(CFRef&& other) noexcept : ref_(std::exchange(other.ref_, nullptr)) {}
    CFRef& operator=(CFRef&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.ref_, nullptr));
        }
        return *this;
    }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;

    [[nodiscard]] T get() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }
    T release() noexcept { return std::exchange(ref_, nullptr); }
    void reset(T ref = nullptr) noexcept {
        if (ref_ != nullptr) {
            CFRelease(ref_);
        }
        ref_ = ref;
    }

private:
    T ref_ = nullptr;
};

}  // namespace kcf::macos
