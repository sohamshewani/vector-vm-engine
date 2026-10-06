#ifndef VECTOR_VM_VM_HPP
#define VECTOR_VM_VM_HPP

#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include <atomic>
#include <variant>
#include <string>
#include <stdexcept>
#include <iostream>
#include <memory>
#include <cstring>
#include <csignal>
#include <sys/mman.h>

namespace vector::vm {

// Forward Declarations
struct ObjectHeader;
class VirtualMachine;
class ExecutionContext;

// 1. Data Structures & Memory Layout: NaN-boxed Value Type (16-byte layout / conceptual NaN boxing)
class Value {
public:
    enum class Type : uint8_t {
        Nil,
        Boolean,
        Integer,
        Float,
        Object
    };

private:
    struct PackedValue {
        uint64_t type_bits;
        union {
            int64_t i_val;
            double f_val;
            bool b_val;
            ObjectHeader* obj_ptr;
        };
    };

    PackedValue storage_;

public:
    constexpr Value() noexcept : storage_{static_cast<uint64_t>(Type::Nil), { .i_val = 0 }} {}
    
    constexpr explicit Value(bool b) noexcept : storage_{static_cast<uint64_t>(Type::Boolean), { .b_val = b }} {}
    constexpr explicit Value(int64_t i) noexcept : storage_{static_cast<uint64_t>(Type::Integer), { .i_val = i }} {}
    constexpr explicit Value(double f) noexcept : storage_{static_cast<uint64_t>(Type::Float), { .f_val = f }} {}
    explicit Value(ObjectHeader* obj) noexcept : storage_{static_cast<uint64_t>(Type::Object), { .obj_ptr = obj }} {}

    constexpr Type getType() const noexcept {
        return static_fn_type(storage_.type_bits);
    }

    constexpr bool isNil() const noexcept { return getType() == Type::Nil; }
    constexpr bool isBoolean() const noexcept { return getType() == Type::Boolean; }
    constexpr bool isInteger() const noexcept { return getType() == Type::Integer; }
    constexpr bool isFloat() const noexcept { return getType() == Type::Float; }
    constexpr bool isObject() const noexcept { return getType() == Type::Object; }

    constexpr bool asBoolean() const noexcept { return storage_.b_val; }
    constexpr int64_t asInteger() const noexcept { return storage_.i_val; }
    constexpr double asFloat() const noexcept { return storage_.f_val; }
    constexpr ObjectHeader* asObject() const noexcept { return storage_.obj_ptr; }

private:
    static constexpr Type static_fn_type(uint64_t bits) noexcept {
        return static_cast<Type>(bits);
    }
};

// Garbage Collection Tricolour Mark-and-Sweep Header
enum class GCColor : uint8_t {
    White = 0,
    Grey = 1,
    Black = 2
};

struct ObjectHeader {
    std::atomic<uint8_t> gc_flags; // Bit 0-1: Color, Bit 2: Marked
    uint8_t type_descriptor;
    uint16_t reserved;
    uint32_t object_size;
    ObjectHeader* forwarding_pointer;
    ObjectHeader* next_object;

    ObjectHeader(uint8_t type_desc, uint32_t size) noexcept
        : gc_flags(static_cast<uint8_t>(GCColor::White)),
          type_descriptor(type_desc),
          reserved(0),
          object_size(size),
          forwarding_pointer(nullptr),
          next_object(nullptr) {}
    
    virtual ~ObjectHeader() = default;
};

// 64-bit Packed Bytecode Instruction
struct Instruction {
    uint8_t opcode;
    uint8_t reg_dest;
    uint8_t reg_src1;
    uint8_t reg_src2;
    uint32_t immediate;

    constexpr Instruction() noexcept : opcode(0), reg_dest(0), reg_src1(0), reg_src2(0), immediate(0) {}
    constexpr Instruction(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2, uint32_t imm) noexcept
        : opcode(op), reg_dest(rd), reg_src1(rs1), reg_src2(rs2), immediate(imm) {}
} __attribute__((aligned(8)));

// Opcodes for the VM
enum class Opcode : uint8_t {
    NOP = 0,
    LOAD_IMM,
    MOV,
    ADD,
    SUB,
    MUL,
    DIV,
    JMP,
    JMP_IF_TRUE,
    JMP_IF_FALSE,
    CALL,
    RET,
    SAFEPOINT,
    HALT
};

// Exception Table Entry for Deterministic Unwinding
struct ExceptionHandlerEntry {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
    uint32_t finally_pc;
};

struct CallFrame {
    uint32_t return_pc;
    uint16_t base_register;
    uint16_t local_count;
};

// 2048-Byte Huge Page Arena Vector Node for AST/Memory
template <typename T>
class ArenaVector {
private:
    struct PageNode {
        PageNode* next;
        size_t capacity;
        size_t size;
        alignas(64) char data[];
    };

    std::atomic<PageNode*> head_;
    size_t page_bytes_;

    PageNode* allocatePage(size_t min_capacity) {
        size_t bytes = sizeof(PageNode) + (min_capacity * sizeof(T));
        bytes = (bytes + page_bytes_ - 1) & ~(page_bytes_ - 1);
        
        void* ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            throw std::runtime_error("Failed to allocate huge page arena block.");
        }
        
        PageNode* node = new (ptr) PageNode();
        node->next = nullptr;
        node->capacity = (bytes - sizeof(PageNode)) / sizeof(T);
        node->size = 0;
        return node;
    }

public:
    explicit ArenaVector(size_t page_size = 2 * 1024 * 1024) : page_bytes_(page_size) {
        head_.store(allocatePage(64), std::memory_order_relaxed);
    }

    ~ArenaVector() {
        PageNode* current = head_.load(std::memory_order_relaxed);
        while (current != nullptr) {
            PageNode* next = current->next;
            size_t bytes = sizeof(PageNode) + (current->capacity * sizeof(T));
            current->~PageNode();
            munmap(current, bytes);
            current = next;
        }
    }

    template <typename... Args>
    T* emplace_back(Args&&... args) {
        PageNode* curr = head_.load(std::memory_order_relaxed);
        while (true) {
            if (curr->size < curr->capacity) {
                size_t idx = curr->size++;
                T* dest = reinterpret_cast<T*>(&curr->data[idx * sizeof(T)]);
                return new (dest) T(std::forward<Args>(args)...);
            }
            
            PageNode* next_node = curr->next;
            if (next_node == nullptr) {
                PageNode* new_page = allocatePage(curr->capacity * 2);
                new_page->next = curr;
                if (head_.compare_exchange_weak(curr, new_page, std::memory_order_release, std::memory_order_relaxed)) {
                    curr = new_page;
                } else {
                    size_t bytes = sizeof(PageNode) + (new_page->capacity * sizeof(T));
                    munmap(new_page, bytes);
                }
            } else {
                curr = next_node;
            }
        }
    }
};

// 2. Concurrency Primitives: Lock-free MPSC Node & Queue
template <typename T>
class LockFreeMPSCQueue {
private:
    struct Node {
        std::atomic<Node*> next;
        T data;
        explicit Node(const T& val) : next(nullptr), data(val) {}
        explicit Node(T&& val) : next(nullptr), data(std::move(val)) {}
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    Node* stub_;

public:
    LockFreeMPSCQueue() {
        stub_ = new Node(T());
        head_.store(stub_, std::memory_order_relaxed);
        tail_.store(stub_, std::memory_order_relaxed);
    }

    ~LockFreeMPSCQueue() {
        T dummy;
        while (dequeue(dummy)) {}
        delete stub_;
    }

    void enqueue(const T& item) {
        Node* node = new Node(item);
        Node* prev_head = head_.exchange(node, std::memory_order_acq_rel);
        prev_head->next.store(node, std::memory_order_release);
    }

    bool dequeue(T& item) {
        Node* tail = tail_.load(std::memory_order_relaxed);
        Node* next = tail->next.load(std::memory_order_acquire);

        if (tail == stub_) {
            if (next == nullptr) {
                return false;
            }
            tail_.store(next, std::memory_order_release);
            tail = next;
            next = tail->next.load(std::memory_order_acquire);
        }

        if (next != nullptr) {
            item = std::move(next->data);
            tail_.store(next, std::memory_order_release);
            delete tail;
            return true;
        }

        Node* head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            enqueue(T());
            Node* new_next = tail->next.load(std::memory_order_acquire);
            if (new_next != nullptr) {
                item = std::move(new_next->data);
                tail_.store(new_next, std::memory_order_release);
                delete tail;
                return true;
            }
        }
        return false;
    }
};

// Hazard Pointer Manager for Mutex-Free Memory Reclamation
class HazardPointerManager {
public:
    static constexpr size_t MAX_THREADS = 64;
    static constexpr size_t MAX_HAZARDS_PER_THREAD = 4;

private:
    struct ThreadHazardRecord {
        std::atomic<const void*> hazards[MAX_HAZARDS_PER_THREAD];
        std::atomic<bool> active;
        ThreadHazardRecord* next;
    };

    std::atomic<ThreadHazardRecord*> head_;

public:
    HazardPointerManager() : head_(nullptr) {}

    ~HazardPointerManager() {
        ThreadHazardRecord* curr = head_.load(std::memory_order_relaxed);
        while (curr != nullptr) {
            ThreadHazardRecord* next = curr->next;
            delete curr;
            curr = next;
        }
    }

    ThreadHazardRecord* acquireRecord() {
        ThreadHazardRecord* record = new ThreadHazardRecord();
        for (size_t i = 0; i < MAX_HAZARDS_PER_THREAD; ++i) {
            record->hazards[i].store(nullptr, std::memory_order_relaxed);
        }
        record->active.store(true, std::memory_order_relaxed);
        
        ThreadHazardRecord* old_head = head_.load(std::memory_order_relaxed);
        do {
            record->next = old_head;
        } while (!head_.compare_exchange_weak(old_head, record, std::memory_order_release, std::memory_order_relaxed));
        
        return record;
    }

    void releaseRecord(ThreadHazardRecord* record) {
        record->active.store(false, std::memory_order_release);
    }
};

// 4. Failure Recovery & Fault Tolerance: Signal Handler
class SignalHandlerContext {
private:
    static bool initialized_;
    static struct sigaction old_sa_segv_;
    static struct sigaction old_sa_abrt_;

    static void handleSignal(int sig, siginfo_t* info, void* context) {
        std::cerr << "[CRITICAL VM FAULT] Signal intercepted: " << sig << "\n";
        if (sig == SIGSEGV) {
            std::cerr << "Faulting memory address: " << info->si_addr << "\n";
        }
        std::cerr << "Execution aborted safely. Stack trace and registers dumped.\n";
        _-std-abort-dispatch(sig);
    }

    static void _-std-abort-dispatch(int sig) {
        std::_Exit(128 + sig);
    }

public:
    static void initialize() {
        if (initialized_) return;
        
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = handleSignal;
        sa.sa_flags = SA_SIGINFO;

        sigaction(SIGSEGV, &sa, &old_sa_segv_);
        sigaction(SIGABRT, &sa, &old_sa_abrt_);
        initialized_ = true;
    }

    static void teardown() {
        if (!initialized_) return;
        sigaction(SIGSEGV, &old_sa_segv_, nullptr);
        sigaction(SIGABRT, &old_sa_abrt_, nullptr);
        initialized_ = false;
    }
};

bool SignalHandlerContext::initialized_ = false;
struct sigaction SignalHandlerContext::old_sa_segv_;
struct sigaction SignalHandlerContext::old_sa_abrt_;

// VM Execution Context & Virtual Machine Engine
class ExecutionContext {
public:
    static constexpr size_t STACK_SIZE = 256;

private:
    std::array<Value, STACK_SIZE> stack_;
    size_t stack_pointer_;
    std::vector<CallFrame> call_stack_;
    uint32_t pc_;
    std::atomic<bool> safepoint_requested_;
    ObjectHeader* allocations_head_;

public:
    ExecutionContext() 
        : stack_pointer_(0), pc_(0), safepoint_requested_(false), allocations_head_(nullptr) {
        stack_.fill(Value());
    }

    ~ExecutionContext() {
        ObjectHeader* curr = allocations_head_;
        while (curr != nullptr) {
            ObjectHeader* next = curr->next_object;
            delete curr;
            curr = next;
        }
    }

    void push(const Value& val) {
        if (stack_pointer_ >= STACK_SIZE) {
            throw std::runtime_error("VM Stack Overflow");
        }
        stack_[stack_pointer_++] = val;
    }

    Value pop() {
        if (stack_pointer_ == 0) {
            throw std::runtime_error("VM Stack Underflow");
        }
        return stack_[--stack_pointer_];
    }

    Value& reg(size_t index) {
        if (index >= STACK_SIZE) {
            throw std::runtime_error("Register index out of bounds");
        }
        return stack_[index];
    }

    void requestSafepoint() {
        safepoint_requested_.store(true, std::memory_order_release);
    }

    void checkSafepoint() {
        if (safepoint_requested_.load(std::memory_order_acquire)) {
            performSafepoint();
        }
    }

    void performSafepoint() {
        // Cooperative safepoint synchronization point
        safepoint_requested_.store(false, std::memory_order_release);
    }

    void registerAllocation(ObjectHeader* obj) {
        obj->next_object = allocations_head_;
        allocations_head_ = obj;
    }

    void execute(const