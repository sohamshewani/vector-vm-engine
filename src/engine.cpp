#include "engine.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <array>
#include <variant>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <exception>

namespace vector_vm {

// Global signal handler context for crash dumping
static thread_local ExecutionContext* current_execution_context = nullptr;

static void VMConsoleSignalHandler(int signal_number, siginfo_t* info, void* context) {
    std::fprintf(stderr, "\n[FATAL VM ERROR] Caught signal %d (fault address: %p)\n", 
                 signal_number, info ? info->si_addr : nullptr);
    
    if (current_execution_context != nullptr) {
        std::fprintf(stderr, "Dumping VM Execution Context:\n");
        std::fprintf(stderr, "  Instruction Pointer (PC): %zu\n", current_execution_context->pc);
        std::fprintf(stderr, "  Active Stack Depth: %zu\n", current_execution_context->stack_pointer);
        std::fprintf(stderr, "  Call Stack Depth: %zu\n", current_execution_context->call_stack_depth);
        
        for (size_t i = 0; i < current_execution_context->stack_pointer && i < 16; ++i) {
            std::fprintf(stderr, "  Reg[%zu] tag=%d\n", i, static_cast<int>(current_execution_context->registers[i].GetType()));
        }
    } else {
        std::fprintf(stderr, "No active execution context registered on this thread.\n");
    }
    
    std::fflush(stderr);
    std::_Exit(128 + signal_number);
}

void InitializeSignalHandlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = VMConsoleSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}

// Arena Allocator implementation
ArenaAllocator::ArenaAllocator(size_t block_size) 
    : block_size_(block_size), current_block_(nullptr), total_allocated_(0) {
    AllocateNewBlock();
}

ArenaAllocator::~ArenaAllocator() {
    std::lock_guard<std::mutex> lock(mutex_);
    ArenaBlock* curr = current_block_;
    while (curr != nullptr) {
        ArenaBlock* next = curr->next;
        std::free(curr->memory);
        delete curr;
        curr = next;
    }
}

void ArenaAllocator::AllocateNewBlock() {
    void* mem = std::malloc(block_size_);
    if (!mem) {
        throw std::bad_alloc();
    }
    ArenaBlock* new_block = new ArenaBlock{mem, block_size_, sizeof(ArenaBlock), current_block_};
    current_block_ = new_block;
    total_allocated_ += block_size_;
}

void* ArenaAllocator::Allocate(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t current_offset = current_block_->offset;
    uintptr_t current_address = reinterpret_cast<uintptr_t>(current_block_->memory) + current_offset;
    size_t adjustment = (alignment - (current_address % alignment)) % alignment;
    size_t needed = size + adjustment;
    
    if (current_block_->capacity < current_offset + needed) {
        AllocateNewBlock();
        current_offset = current_block_->offset;
        current_address = reinterpret_cast<uintptr_t>(current_block_->memory) + current_offset;
        adjustment = (alignment - (current_address % alignment)) % alignment;
        needed = size + adjustment;
    }
    
    void* ptr = reinterpret_cast<void*>(current_address + adjustment);
    current_block_->offset += needed;
    return ptr;
}

void ArenaAllocator::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ArenaBlock* curr = current_block_;
    while (curr != nullptr) {
        curr->offset = sizeof(ArenaBlock);
        curr = curr->next;
    }
}

// Garbage Collector Implementation
GarbageCollector::GarbageCollector() : bytes_allocated_(0), next_gc_threshold_(1024 * 1024) {}

GarbageCollector::~GarbageCollector() {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    ObjectHeader* curr = object_list_head_;
    while (curr != nullptr) {
        ObjectHeader* next = curr->next;
        std::free(curr);
        curr = next;
    }
}

void* GarbageCollector::AllocateObject(size_t size, ObjectType type) {
    size_t total_size = sizeof(ObjectHeader) + size;
    {
        std::lock_guard<std::mutex> lock(gc_mutex_);
        if (bytes_allocated_ > next_gc_threshold_) {
            PerformGC();
        }
        bytes_allocated_ += total_size;
    }
    
    void* raw = std::malloc(total_size);
    if (!raw) {
        throw std::bad_alloc();
    }
    
    ObjectHeader* header = new (raw) ObjectHeader();
    header->flags = GCFlags::White;
    header->type = type;
    header->size = size;
    header->forwarding_pointer = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(gc_mutex_);
        header->next = object_list_head_;
        object_list_head_ = header;
    }
    
    return reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(header) + sizeof(ObjectHeader));
}

void GarbageCollector::RegisterRoot(Value* root_slot) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    roots_.push_back(root_slot);
}

void GarbageCollector::UnregisterRoot(Value* root_slot) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    for (auto it = roots_.begin(); it != roots_.end(); ++it) {
        if (*it == root_slot) {
            roots_.erase(it);
            break;
        }
    }
}

void GarbageCollector::PerformGC() {
    // Mark Phase
    for (Value* root : roots_) {
        if (root && root->IsObject()) {
            MarkObject(root->GetObjectHeader());
        }
    }
    
    // Sweep Phase
    ObjectHeader** curr = &object_list_head_;
    while (*curr != nullptr) {
        ObjectHeader* obj = *curr;
        if (obj->flags == GCFlags::White) {
            *curr = obj->next;
            bytes_allocated_ -= (sizeof(ObjectHeader) + obj->size);
            std::free(obj);
        } else {
            if (obj->flags == GCFlags::Black) {
                obj->flags = GCFlags::White;
            }
            curr = &(obj->next);
        }
    }
    
    next_gc_threshold_ = bytes_allocated_ * 2;
    if (next_gc_threshold_ < 1024 * 1024) {
        next_gc_threshold_ = 1024 * 1024;
    }
}

void GarbageCollector::MarkObject(ObjectHeader* obj) {
    if (!obj || obj->flags == GCFlags::Black || obj->flags == GCFlags::Grey) {
        return;
    }
    obj->flags = GCFlags::Grey;
    
    // Process internal object fields if it's an array or map
    if (obj->type == ObjectType::Array) {
        Value* elements = reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(obj) + sizeof(ObjectHeader));
        size_t count = obj->size / sizeof(Value);
        for (size_t i = 0; i < count; ++i) {
            if (elements[i].IsObject()) {
                MarkObject(elements[i].GetObjectHeader());
            }
        }
    }
    
    obj->flags = GCFlags::Black;
}

// Work-Stealing Queue Implementation
WorkStealingQueue::WorkStealingQueue(size_t capacity) : capacity_(capacity), mask_(capacity - 1) {
    if ((capacity & mask_) != 0) {
        throw std::invalid_argument("Capacity must be a power of two");
    }
    buffer_ = new std::atomic<Instruction*>[capacity_];
    for (size_t i = 0; i < capacity_; ++i) {
        buffer_[i].store(nullptr, std::memory_order_relaxed);
    }
}

WorkStealingQueue::~WorkStealingQueue() {
    delete[] buffer_;
}

bool WorkStealingQueue::Push(Instruction* item) {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_acquire);
    if (b - t >= static_cast<int64_t>(capacity_)) {
        return false; // Queue full
    }
    buffer_[b & mask_].store(item, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
    return true;
}

Instruction* WorkStealingQueue::Pop() {
    int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top_.load(std::memory_order_relaxed);
    if (t <= b) {
        Instruction* item = buffer_[b & mask_].load(std::memory_order_relaxed);
        if (t == b) {
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                bottom_.store(b + 1, std::memory_order_relaxed);
                return nullptr;
            }
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return item;
    } else {
        bottom_.store(b + 1, std::memory_order_relaxed);
        return nullptr;
    }
}

Instruction* WorkStealingQueue::Steal() {
    int64_t t = top_.load(std::memory_order_acquire);
    int64_t b = bottom_.load(std::memory_order_acquire);
    if (t < b) {
        Instruction* item = buffer_[t & mask_].load(std::memory_order_relaxed);
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return nullptr;
        }
        return item;
    }
    return nullptr;
}

// Hazard Pointer Manager
HazardPointerManager& HazardPointerManager::GetInstance() {
    static HazardPointerManager instance;
    return instance;
}

HazardPointerManager::HazardPointerManager() {}

HazardPointerManager::~HazardPointerManager() {
    HazardPointer* curr = head_.load(std::memory_order_relaxed);
    while (curr) {
        HazardPointer* next = curr->next;
        delete curr;
        curr = next;
    }
}

HazardPointer* HazardPointerManager::Acquire() {
    HazardPointer* hp = head_.load(std::memory_order_acquire);
    while (hp) {
        void* expected = nullptr;
        if (hp->pointer.compare_exchange_strong(expected, reinterpret_cast<void*>(1), std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return hp;
        }
        hp = hp->next;
    }
    
    HazardPointer* new_hp = new HazardPointer();
    new_hp->pointer.store(reinterpret_cast<void*>(1), std::memory_order_relaxed);
    new_hp->next = head_.load(std::memory_order_relaxed);
    while (!head_.compare_exchange_weak(new_hp->next, new_hp, std::memory_order_release, std::memory_order_relaxed)) {}
    return new_hp;
}

void HazardPointerManager::Release(HazardPointer* hp) {
    hp->pointer.store(nullptr, std::memory_order_release);
}

// ExecutionContext Implementation
ExecutionContext::ExecutionContext(GarbageCollector& gc) 
    : stack_pointer(0), pc(0), call_stack_depth(0), gc_(gc) {
    registers.fill(Value());
}

ExecutionContext::~ExecutionContext() {
    for (size_t i = 0; i < stack_pointer; ++i) {
        if (registers[i].IsObject()) {
            gc_.UnregisterRoot(&registers[i]);
        }
    }
}

void ExecutionContext::PushStack(Value val) {
    if (stack_pointer >= 256) {
        throw std::runtime_error("VM Stack Overflow");
    }
    if (val.IsObject()) {
        gc_.RegisterRoot(&registers[stack_pointer]);
    }
    registers[stack_pointer++] = val;
}

Value ExecutionContext::PopStack() {
    if (stack_pointer == 0) {
        throw std::runtime_error("VM Stack Underflow");
    }
    stack_pointer--;
    Value val = registers[stack_pointer];
    if (val.IsObject()) {
        gc_.UnregisterRoot(&registers[stack_pointer]);
    }
    registers[stack_pointer] = Value();
    return val;
}

void ExecutionContext::PushExceptionHandler(size_t try_pc, size_t catch_pc, size_t finally_pc) {
    if (call_stack_depth >= 256) {
        throw std::runtime_error("Call Stack Overflow");
    }
    exception_handlers[call_stack_depth++] = ExceptionHandlerTable{try_pc, catch_pc, finally_pc, stack_pointer};
}

void ExecutionContext::PopExceptionHandler() {
    if (call_stack_depth == 0) {
        throw std::runtime_error("Call Stack Underflow");
    }
    call_stack_depth--;
}

bool ExecutionContext::UnwindException() {
    if (call_stack_depth == 0) {
        return false;
    }
    const auto& handler = exception_handlers[call_stack_depth - 1];
    pc = handler.catch_pc;
    stack_pointer = handler.saved_stack_pointer;
    return true;
}

// VM Engine Main Implementation
VMEngine::VMEngine(size_t worker_threads) 
    : worker_threads_(worker_threads), running_(false) {
    InitializeSignalHandlers();
}

VMEngine::~VMEngine() {
    Stop();
}

void VMEngine::Start() {
    running_.store(true, std::memory_order_release);
    for (size_t i = 0; i < worker_threads_; ++i) {
        queues_.push_back(std::make_unique<WorkStealingQueue>(256));
        workers_.emplace_back(&VMEngine::WorkerLoop, this, i);
    }
}

void VMEngine::Stop() {
    running_.store(false, std::memory_order_release);
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    queues_.clear();
}

void VMEngine::SubmitTask(Instruction* instr) {
    if (queues_.empty()) {
        throw std::runtime_error("VM Engine not started");
    }
    static thread_local size_t counter = 0;
    size_t q_idx = counter++ % queues_.size();
    while (!queues_[q_idx]->Push(