#ifndef VECTOR_VM_TYPES_HPP
#define VECTOR_VM_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <variant>
#include <type_traits>
#include <new>
#include <utility>
#include <system_error>

namespace vector::vm {

// Forward declarations
struct ObjectHeader;
class Value;

// ============================================================================
// 1. DATA STRUCTURES & MEMORY LAYOUT: Bytecode Instruction
// ============================================================================

enum class Opcode : uint8_t {
    Nop = 0x00,
    LoadConst = 0x01,
    Move = 0x02,
    Add = 0x03,
    Sub = 0x04,
    Mul = 0x05,
    Div = 0x06,
    Jmp = 0x07,
    JmpIfTrue = 0x08,
    JmpIfFalse = 0x09,
    Call = 0x0a,
    Ret = 0x0b,
    Safepoint = 0x0c,
    Halt = 0xff
};

// Fixed 64-bit aligned packed instruction word
struct alignas(8) Instruction {
    uint8_t opcode;
    uint8_t r0;
    uint8_t r1;
    uint8_t r2;
    uint32_t immediate;

    constexpr Instruction() noexcept 
        : opcode(static_cast<uint8_t>(Opcode::Nop)), r0(0), r1(0), r2(0), immediate(0) {}

    constexpr Instruction(Opcode op, uint8_t reg0, uint8_t reg1, uint8_t reg2, uint32_t imm = 0) noexcept
        : opcode(static_cast<uint8_t>(op)), r0(reg0), r1(reg1), r2(reg2), immediate(imm) {}

    [[nodiscard]] constexpr Opcode get_opcode() const noexcept {
        return static_cast<Opcode>(opcode);
    }
} __attribute__((packed));

static_assert(sizeof(Instruction) == 8, "Instruction must be exactly 64 bits");
static_assert(std::alignment_of_v<Instruction> == 8, "Instruction must be 8-byte aligned");

// ============================================================================
// 3. SYSTEM INVARIANTS: NaN-Boxed Value Representation (16-byte layout variant)
// ============================================================================

enum class TypeTag : uint64_t {
    Double  = 0x0000,
    Int64   = 0xFFF1,
    Boolean = 0xFFF2,
    Nil     = 0xFFF3,
    Object  = 0xFFF4
};

class alignas(16) Value {
private:
    union Storage {
        double d;
        int64_t i;
        bool b;
        ObjectHeader* obj;
        uint64_t raw;
    } storage_;

    TypeTag tag_;

public:
    constexpr Value() noexcept : storage_{.raw = 0}, tag_(TypeTag::Nil) {}
    
    constexpr Value(double val) noexcept : storage_{.d = val}, tag_(TypeTag::Double) {}
    constexpr Value(int64_t val) noexcept : storage_{.i = val}, tag_(TypeTag::Int64) {}
    constexpr Value(bool val) noexcept : storage_{.b = val}, tag_(TypeTag::Boolean) {}
    
    explicit constexpr Value(nullptr_t) noexcept : storage_{.raw = 0}, tag_(TypeTag::Nil) {}
    explicit Value(ObjectHeader* obj_ptr) noexcept : storage_{.obj = obj_ptr}, tag_(TypeTag::Object) {}

    [[nodiscard]] constexpr TypeTag get_tag() const noexcept { return tag_; }
    
    [[nodiscard]] constexpr bool is_double() const noexcept { return tag_ == TypeTag::Double; }
    [[nodiscard]] constexpr bool is_int64() const noexcept { return tag_ == TypeTag::Int64; }
    [[nodiscard]] constexpr bool is_boolean() const noexcept { return tag_ == TypeTag::Boolean; }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return tag_ == TypeTag::Nil; }
    [[nodiscard]] constexpr bool is_object() const noexcept { return tag_ == TypeTag::Object; }

    [[nodiscard]] double as_double() const noexcept { return storage_.d; }
    [[nodiscard]] int64_t as_int64() const noexcept { return storage_.i; }
    [[nodiscard]] bool as_boolean() const noexcept { return storage_.b; }
    [[nodiscard]] ObjectHeader* as_object() const noexcept { return storage_.obj; }

    Value(const Value&) = default;
    Value& operator=(const Value&) = default;
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;

    ~Value() = default;
} __attribute__((aligned(16)));

static_assert(sizeof(Value) == 16, "Value must be 16 bytes for aligned layout");

// ============================================================================
// 1. DATA STRUCTURES & MEMORY LAYOUT: Garbage Collection Object Header
// ============================================================================

enum class GCColor : uint8_t {
    White = 0,
    Grey = 1,
    Black = 2
};

struct ObjectHeader {
    std::atomic<uint32_t> gc_flags; // Bitfield: [Color:2 | Marked:1 | TypeDescriptor:29]
    ObjectHeader* forwarding_pointer;
    ObjectHeader* next_intrusive;

    constexpr ObjectHeader(uint32_t type_desc) noexcept
        : gc_flags(type_desc & 0x1FFFFFFF),
          forwarding_pointer(nullptr),
          next_intrusive(nullptr) {}

    [[nodiscard]] GCColor get_color() const noexcept {
        uint32_t flags = gc_flags.load(std::memory_order_relaxed);
        return static_cast<GCColor>((flags >> 30) & 0x03);
    }

    void set_color(GCColor color) noexcept {
        uint32_t current = gc_flags.load(std::memory_order_relaxed);
        uint32_t updated;
        do {
            updated = (current & ~(0x03 << 30)) | (static_cast<uint32_t>(color) << 30);
        } while (!gc_flags.compare_exchange_weak(current, updated, std::memory_order_release, std::memory_order_relaxed));
    }

    [[nodiscard]] bool is_marked() const noexcept {
        return (gc_flags.load(std::memory_order_relaxed) & (1 << 29)) != 0;
    }

    void set_marked(bool marked) noexcept {
        uint32_t current = gc_flags.load(std::memory_order_relaxed);
        uint32_t updated;
        do {
            if (marked) {
                updated = current | (1 << 29);
            } else {
                updated = current & ~(1 << 29);
            }
        } while (!gc_flags.compare_exchange_weak(current, updated, std::memory_order_release, std::memory_order_relaxed));
    }

    [[nodiscard]] uint32_t get_type_descriptor() const noexcept {
        return gc_flags.load(std::memory_order_relaxed) & 0x1FFFFFFF;
    }
};

// ============================================================================
// 1. DATA STRUCTURES & MEMORY LAYOUT: Arena Vector utilizing 2MB Huge Pages
// ============================================================================

class ArenaMemoryResource {
private:
    struct Chunk {
        char* memory;
        size_t capacity;
        size_t used;
        Chunk* next;
    };

    Chunk* head_;
    size_t chunk_size_;

    void allocate_chunk(size_t min_size) {
        // Enforce 2MB huge page chunk sizing standard
        size_t alloc_size = (min_size > 2 * 1024 * 1024) ? min_size : (2 * 1024 * 1024);
        
        // System allocation hook for 2MB pages (Platform abstraction simplified to aligned malloc)
        void* raw_mem = ::operator new(alloc_size, std::align_val_t{2 * 1024 * 1024});
        
        auto* new_chunk = static_cast<Chunk*>(::operator new(sizeof(Chunk)));
        new_chunk->memory = static_cast<char*>(raw_mem);
        new_chunk->capacity = alloc_size;
        new_chunk->used = 0;
        new_chunk->next = head_;
        head_ = new_chunk;
    }

public:
    explicit ArenaMemoryResource(size_t initial_chunk_size = 2 * 1024 * 1024) 
        : head_(nullptr), chunk_size_(initial_chunk_size) {
        allocate_chunk(chunk_size_);
    }

    ~ArenaMemoryResource() {
        Chunk* current = head_;
        while (current != nullptr) {
            Chunk* next = current->next;
            ::operator delete(current->memory, std::align_val_t{2 * 1024 * 1024});
            ::operator delete(current);
            current = next;
        }
    }

    ArenaMemoryResource(const ArenaMemoryResource&) = delete;
    ArenaMemoryResource& operator=(const ArenaMemoryResource&) = delete;
    ArenaMemoryResource(ArenaMemoryResource&&) noexcept = delete;
    ArenaMemoryResource& operator=(ArenaMemoryResource&&) noexcept = delete;

    [[nodiscard]] void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        size_t space = head_->capacity - head_->used;
        void* ptr = head_->memory + head_->used;
        
        if (std::align(alignment, bytes, ptr, space) == nullptr) {
            allocate_chunk(bytes + alignment);
            space = head_->capacity - head_->used;
            ptr = head_->memory + head_->used;
            if (std::align(alignment, bytes, ptr, space) == nullptr) {
                throw std::bad_alloc();
            }
        }
        
        size_t aligned_offset = static_cast<char*>(ptr) - head_->memory;
        head_->used = aligned_offset + bytes;
        return ptr;
    }

    void reset() noexcept {
        Chunk* current = head_->next;
        while (current != nullptr) {
            Chunk* next = current->next;
            ::operator delete(current->memory, std::align_val_t{2 * 1024 * 1024});
            ::operator delete(current);
            current = next;
        }
        if (head_) {
            head_->next = nullptr;
            head_->used = 0;
        }
    }
};

template <typename T>
class ArenaVector {
private:
    ArenaMemoryResource* arena_;
    T* data_;
    size_t size_;
    size_t capacity_;

public:
    explicit ArenaVector(ArenaMemoryResource* arena, size_t initial_capacity = 16)
        : arena_(arena), size_(0), capacity_(initial_capacity) {
        if (capacity_ == 0) capacity_ = 16;
        data_ = static_cast<T*>(arena_->allocate(capacity_ * sizeof(T), alignof(T)));
    }

    ~ArenaVector() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < size_; ++i) {
                data_[i].~T();
            }
        }
    }

    ArenaVector(const ArenaVector&) = delete;
    ArenaVector& operator=(const ArenaVector&) = delete;
    ArenaVector(ArenaVector&& other) noexcept
        : arena_(other.arena_), data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    ArenaVector& operator=(ArenaVector&& other) noexcept {
        if (this != &other) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (size_t i = 0; i < size_; ++i) {
                    data_[i].~T();
                }
            }
            arena_ = other.arena_;
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    void push_back(const T& value) {
        if (size_ >= capacity_) {
            resize_capacity(capacity_ * 2);
        }
        ::new (static_cast<void*>(data_ + size_)) T(value);
        ++size_;
    }

    void push_back(T&& value) {
        if (size_ >= capacity_) {
            resize_capacity(capacity_ * 2);
        }
        ::new (static_cast<void*>(data_ + size_)) T(std::move(value));
        ++size_;
    }

    template <typename... Args>
    void emplace_back(Args&&... args) {
        if (size_ >= capacity_) {
            resize_capacity(capacity_ * 2);
        }
        ::new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        ++size_;
    }

    [[nodiscard]] T& operator[](size_t index) noexcept { return data_[index]; }
    [[nodiscard]] const T& operator[](size_t index) const noexcept { return data_[index]; }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }

    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return data_ + size_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }

private:
    void resize_capacity(size_t new_capacity) {
        T* new_data = static_cast<T*>(arena_->allocate(new_capacity * sizeof(T), alignof(T)));
        for (size_t i = 0; i < size_; ++i) {
            ::new (static_cast<void*>(new_data + i)) T(std::move(data_[i]));
            if constexpr (!std::is_trivially_destructible_v<T>) {
                data_[i].~T();
            }
        }
        data_ = new_data;
        capacity_ = new_capacity;
    }
};

// ============================================================================
// 3. SYSTEM INVARIANTS: VM Execution Context & Register Stack
// ============================================================================

struct ExecutionContext {
    // Fixed pre-allocated register stack eliminating heap operations
    std::array<Value, 256> registers;
    
    // Instruction Pointer & Program Counter ranges
    const Instruction* ip;
    const Instruction* code_begin;
    const Instruction* code_end;

    // Exception handling try-catch metadata table reference
    struct ExceptionHandlerEntry {
        uint32_t start_pc;
        uint32_t end_pc;
        uint32_t handler_pc;
    };

    const ExceptionHandlerEntry* handlers;
    size_t handler_count;

    ExecutionContext() noexcept 
        : registers{}, ip(nullptr), code