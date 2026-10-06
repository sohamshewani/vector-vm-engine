#ifndef VECTOR_VM_COMPILER_HPP
#define VECTOR_VM_COMPILER_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <new>
#include <utility>
#include <type_traits>
#include <system_error>
#include <array>
#include <vector>
#include <string_view>

namespace vector::vm {

enum class ErrorCode : uint32_t {
    Success = 0,
    OutOfMemory,
    HugePageAllocationFailed,
    ArenaOverflow,
    CompilationError,
    StackOverflow,
    StackUnderflow,
    InvalidOpcode,
    TypeMismatch,
    NullPointerException,
    SignalCaught
};

class VMException : public std::system_error {
public:
    explicit VMException(ErrorCode ec, std::string_view message = "")
        : std::system_error(static_cast<int>(ec), std::generic_category(), message.data()),
          code_(ec) {}

    [[nodiscard]] ErrorCode get_code() const noexcept {
        return code_;
    }

private:
    ErrorCode code_;
};

class HugePageAllocator {
public:
    static constexpr size_t kPageSize = 2 * 1024 * 1024; // 2MB Huge Page

    [[nodiscard]] static void* allocate(size_t size) {
        size_t aligned_size = (size + kPageSize - 1) & ~(kPageSize - 1);
        if (aligned_size < kPageSize) {
            aligned_size = kPageSize;
        }

#if defined(MAP_HUGETLB) && defined(MAP_ANONYMOUS)
        void* ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr != MAP_FAILED) {
            return ptr;
        }
#endif
        // Fallback to standard aligned allocation if huge pages are unavailable
        void* ptr = nullptr;
        int res = posix_memalign(&ptr, kPageSize, aligned_size);
        if (res != 0 || ptr == nullptr) {
            throw VMException(ErrorCode::HugePageAllocationFailed, "Failed to allocate 2MB huge page memory block.");
        }
        std::memset(ptr, 0, aligned_size);
        return ptr;
    }

    static void deallocate(void* ptr, size_t size) noexcept {
        if (!ptr) return;
        size_t aligned_size = (size + kPageSize - 1) & ~(kPageSize - 1);
        if (aligned_size < kPageSize) {
            aligned_size = kPageSize;
        }
#if defined(MAP_HUGETLB) && defined(MAP_ANONYMOUS)
        munmap(ptr, aligned_size);
#else
        free(ptr);
#endif
    }
};

template <typename T>
class ArenaVector {
public:
    explicit ArenaVector(size_t initial_capacity = 1024)
        : capacity_(initial_capacity), size_(0) {
        size_t bytes = capacity_ * sizeof(T);
        buffer_ = static_cast<T*>(HugePageAllocator::allocate(bytes));
    }

    ~ArenaVector() noexcept {
        clear();
        if (buffer_) {
            HugePageAllocator::deallocate(buffer_, capacity_ * sizeof(T));
            buffer_ = nullptr;
        }
    }

    ArenaVector(const ArenaVector&) = delete;
    ArenaVector& operator=(const ArenaVector&) = delete;

    ArenaVector(ArenaVector&& other) noexcept
        : buffer_(other.buffer_), capacity_(other.capacity_), size_(other.size_) {
        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
    }

    ArenaVector& operator=(ArenaVector&& other) noexcept {
        if (this != &other) {
            clear();
            if (buffer_) {
                HugePageAllocator::deallocate(buffer_, capacity_ * sizeof(T));
            }
            buffer_ = other.buffer_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            other.buffer_ = nullptr;
            other.capacity_ = 0;
            other.size_ = 0;
        }
        return this;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (size_ >= capacity_) {
            grow();
        }
        T* dest = buffer_ + size_;
        ::new (static_cast<void*>(dest)) T(std::forward<Args>(args)...);
        return buffer_[size_++];
    }

    void push_back(const T& value) {
        emplace_back(value);
    }

    void push_back(T&& value) {
        emplace_back(std::move(value));
    }

    [[nodiscard]] T& operator[](size_t index) noexcept {
        return buffer_[index];
    }

    [[nodiscard]] const T& operator[](size_t index) const noexcept {
        return buffer_[index];
    }

    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

    void clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < size_; ++i) {
                buffer_[i].~T();
            }
        }
        size_ = 0;
    }

private:
    void grow() {
        size_t old_bytes = capacity_ * sizeof(T);
        size_t new_capacity = capacity_ * 2;
        size_t new_bytes = new_capacity * sizeof(T);

        T* new_buffer = static_cast<T*>(HugePageAllocator::allocate(new_bytes));
        
        for (size_t i = 0; i < size_; ++i) {
            ::new (static_cast<void*>(new_buffer + i)) T(std::move(buffer_[i]));
            if constexpr (!std::is_trivially_destructible_v<T>) {
                buffer_[i].~T();
            }
        }

        HugePageAllocator::deallocate(buffer_, old_bytes);
        buffer_ = new_buffer;
        capacity_ = new_capacity;
    }

    T* buffer_;
    size_t capacity_;
    size_t size_;
};

enum class Opcode : uint8_t {
    NOP = 0x00,
    LOAD = 0x01,
    STORE = 0x02,
    ADD = 0x03,
    SUB = 0x04,
    MUL = 0x05,
    DIV = 0x06,
    JMP = 0x07,
    JZ = 0x08,
    JNZ = 0x09,
    CALL = 0x0A,
    RET = 0x0B,
    HALT = 0xFF
};

struct alignas(8) Instruction {
    uint8_t opcode;
    uint8_t reg_a;
    uint8_t reg_b;
    uint8_t reg_c;
    uint32_t immediate;

    [[nodiscard]] static constexpr Instruction make(Opcode op, uint8_t a, uint8_t b, uint8_t c, uint32_t imm = 0) noexcept {
        return Instruction{static_cast<uint8_t>(op), a, b, c, imm};
    }
};

static_assert(sizeof(Instruction) == 8, "Instruction must be exactly 64 bits.");
static_assert(alignof(Instruction) == 8, "Instruction must be 64-bit aligned.");

enum class ValueType : uint8_t {
    Nil = 0,
    Boolean,
    Integer,
    Float,
    ObjectPtr
};

struct ObjectHeader;

struct alignas(16) Value {
    union Payload {
        int64_t i;
        double f;
        bool b;
        ObjectHeader* obj;
    } payload;

    ValueType type;
    uint8_t padding[7];

    constexpr Value() noexcept : payload{.i = 0}, type(ValueType::Nil), padding{} {}
    constexpr Value(bool val) noexcept : payload{.b = val}, type(ValueType::Boolean), padding{} {}
    constexpr Value(int64_t val) noexcept : payload{.i = val}, type(ValueType::Integer), padding{} {}
    constexpr Value(double val) noexcept : payload{.f = val}, type(ValueType::Float), padding{} {}
    constexpr Value(ObjectHeader* val) noexcept : payload{.obj = val}, type(ValueType::ObjectPtr), padding{} {}

    [[nodiscard]] constexpr bool is_nil() const noexcept { return type == ValueType::Nil; }
    [[nodiscard]] constexpr bool is_bool() const noexcept { return type == ValueType::Boolean; }
    [[nodiscard]] constexpr bool is_int() const noexcept { return type == ValueType::Integer; }
    [[nodiscard]] constexpr bool is_float() const noexcept { return type == ValueType::Float; }
    [[nodiscard]] constexpr bool is_obj() const noexcept { return type == ValueType::ObjectPtr; }

    [[nodiscard]] constexpr int64_t as_int() const noexcept { return payload.i; }
    [[nodiscard]] constexpr double as_float() const noexcept { return payload.f; }
    [[nodiscard]] constexpr bool as_bool() const noexcept { return payload.b; }
    [[nodiscard]] constexpr ObjectHeader* as_obj() const noexcept { return payload.obj; }
};

static_assert(sizeof(Value) == 16, "Value must be precisely 16 bytes.");

struct ObjectHeader {
    uint32_t gc_flags;
    uint32_t type_descriptor;
    ObjectHeader* forwarding_pointer;
    ObjectHeader* next;

    constexpr ObjectHeader(uint32_t type_desc) noexcept
        : gc_flags(0), type_descriptor(type_desc), forwarding_pointer(nullptr), next(nullptr) {}
};

class CompilationContext {
public:
    CompilationContext() : instructions_(16384) {}
    ~CompilationContext() = default;

    void emit(Opcode op, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint32_t imm = 0) {
        instructions_.push_back(Instruction::make(op, a, b, c, imm));
    }

    [[nodiscard]] const ArenaVector<Instruction>& get_instructions() const noexcept {
        return instructions_;
    }

    [[nodiscard]] size_t get_pc() const noexcept {
        return instructions_.size();
    }

private:
    ArenaVector<Instruction> instructions_;
};

} // namespace vector::vm

#endif // VECTOR_VM_COMPILER_HPP