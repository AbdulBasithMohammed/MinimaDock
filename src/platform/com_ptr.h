#pragma once
#include <utility>
namespace minima {
template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept { if (this != &other) { reset(); ptr_ = std::exchange(other.ptr_, nullptr); } return *this; }
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    T** put() { reset(); return &ptr_; }
    void reset() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
private:
    T* ptr_ = nullptr;
};
}
