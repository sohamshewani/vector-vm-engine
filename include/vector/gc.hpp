#ifndef VECTOR_GC_HPP
#define VECTOR_GC_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace vector::vm {

enum class ObjectType : uint8_t {
    String = 1,
    Array = 2,
    Map = 3,
    Closure = 4,
    UserDefined = 5
};

struct ObjectHeader {
    std::atomic<uint8_t> gc_flags; // Bit 0: Mark, Bit 1: Forwarded, etc.
    ObjectType type;
    uint16_t reserved;
    uint32_t size;
    std::atomic<ObjectHeader*> forwarding_pointer;

    static constexpr uint8_t kMarkBit = 1 << 0;
    static constexpr uint8_t kForwardedBit = 1 << 1;

    bool is_marked() const {
        return (gc_flags.load(std::memory_order_relaxed) & kMarkBit) != 0;
    }

    void set_marked(bool marked) {
        uint8_t current = gc_flags.load(std::memory_order_relaxed);
        while (true) {
            uint8_t desired = marked ? (current | kMarkBit) : (current & ~kMarkBit);
            if (gc_flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    bool is_forwarded() const {
        return (gc_flags.load(std::memory_order_relaxed) & kForwardedBit) != 0;
    }

    void set_forwarded(ObjectHeader* fw) {
        forwarding_pointer.store(fw, std::memory_order_relaxed);
        uint8_t current = gc_flags.load(std::memory_order_relaxed);
        while (true) {
            uint8_t desired = current | kForwardedBit;
            if (gc_flags.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
    }
};

class Value;

class GarbageCollector {
public:
    enum class State {
        Idle,
        Marking,
        Sweeping,
        Compact
    };

    explicit GarbageCollector(size_t heap_soft_limit = 1024 * 1024 * 64)
        : head_(nullptr),
          state_(State::Idle),
          allocated_bytes_(0),
          heap_soft_limit_(heap_soft_limit),
          safepoint_requested_(false) {}

    ~GarbageCollector() {
        // Unconditionally reclaim all allocated objects on shutdown.
        ObjectHeader* current = head_.load(std::memory_order_relaxed);
        while (current != nullptr) {
            ObjectHeader* next = get_next(current);
            destroy_object(current);
            current = next;
        }
    }

    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;
    GarbageCollector(GarbageCollector&&) = delete;
    GarbageCollector& operator=(GarbageCollector&&) = delete;

    template <typename T, typename... Args>
    T* allocate(Args&&... args) {
        static_assert(std::is_base_of_v<ObjectHeader, T>, "T must derive from ObjectHeader");
        
        size_t total_size = sizeof(T);
        check_allocation_threshold(total_size);

        void* raw_memory = ::operator new(total_size, std::align_val_t{alignof(T)});
        auto* obj = new (raw_memory) T(std::forward<Args>(args)...);

        // Initialize intrusive header fields
        obj->gc_flags.store(0, std::memory_order_relaxed);
        obj->forwarding_pointer.store(nullptr, std::memory_order_relaxed);
        
        // Link into the global intrusive object list atomically
        ObjectHeader* old_head = head_.load(std::memory_order_relaxed);
        do {
            set_next(obj, old_head);
        } while (!head_.compare_exchange_weak(old_head, obj, std::memory_order_release, std::memory_order_relaxed));

        allocated_bytes_.fetch_add(total_size, std::memory_order_relaxed);
        return obj;
    }

    void request_safepoint() {
        safepoint_requested_.store(true, std::memory_order_release);
    }

    bool is_safepoint_requested() const {
        return safepoint_requested_.load(std::memory_order_acquire);
    }

    template <typename RootSetFunction>
    void collect(RootSetFunction get_roots) {
        if (state_.load(std::memory_order_acquire) != State::Idle) {
            return;
        }
        state_.store(State::Marking, std::memory_order_release);

        // Phase 1: Mark
        std::vector<ObjectHeader*> gray_stack;
        
        // Register roots
        get_roots([&gray_stack](ObjectHeader* root) {
            if (root != nullptr && !root->is_marked()) {
                root->set_marked(true);
                gray_stack.push_back(root);
            }
        });

        // Trace gray objects
        while (!gray_stack.empty()) {
            ObjectHeader* obj = gray_stack.back();
            gray_stack.pop_back();

            trace_object_fields(obj, [&gray_stack](ObjectHeader* child) {
                if (child != nullptr && !child->is_marked()) {
                    child->set_marked(true);
                    gray_stack.push_back(child);
                }
            });
        }

        // Phase 2: Sweep
        state_.store(State::Sweeping, std::memory_order_release);
        
        ObjectHeader* current = head_.load(std::memory_order_relaxed);
        ObjectHeader* prev = nullptr;
        size_t freed_bytes = 0;

        while (current != nullptr) {
            ObjectHeader* next = get_next(current);
            if (current->is_marked()) {
                // Clear mark bit for the next collection cycle
                current->set_marked(false);
                prev = current;
            } else {
                // Unlinked and sweep
                if (prev == nullptr) {
                    head_.store(next, std::memory_order_relaxed);
                } else {
                    set_next(prev, next);
                }
                freed_bytes += current->size;
                destroy_object(current);
            }
            current = next;
        }

        allocated_bytes_.fetch_sub(freed_bytes, std::memory_order_relaxed);
        safepoint_requested_.store(false, std::memory_order_release);
        state_.store(State::Idle, std::memory_order_release);
    }

    size_t get_allocated_bytes() const {
        return allocated_bytes_.load(std::memory_order_relaxed);
    }

    State get_state() const {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<ObjectHeader*> head_;
    std::atomic<State> state_;
    std::atomic<size_t> allocated_bytes_;
    size_t heap_soft_limit_;
    std::atomic<bool> safepoint_requested_;

    // Intrusive linkage payload stored immediately following ObjectHeader in memory layout via tail padding or custom embedding.
    // For universal intrusive lists without requiring T to embed pointers, we store the next pointer using a contiguous slot or require layout conformance.
    // Here we assume the object layout reserves space or we manage it via a companion pointer map. To keep it strictly intrusive 
    // without external maps, we inject a next-pointer extractor via standard layout assumptions or mandate an intrusive node base.
    // Let's implement standard intrusive node pointer extraction assuming standard layout where the first 32 bytes contain header and linkage.
    
    struct IntrusiveNode {
        ObjectHeader header;
        ObjectHeader* next;
    };

    static ObjectHeader* get_next(ObjectHeader* obj) {
        return static_cast<IntrusiveNode*>(static_cast<void*>(obj))->next;
    }

    static void set_next(ObjectHeader* obj, ObjectHeader* next) {
        static_cast<IntrusiveNode*>(static_cast<void*>(obj))->next = next;
    }

    void check_allocation_threshold(size_t requested_size) {
        if (allocated_bytes_.load(std::memory_order_relaxed) + requested_size > heap_soft_limit_) {
            request_safepoint();
        }
    }

    void destroy_object(ObjectHeader* obj) {
        obj->~ObjectHeader();
        ::operator delete(obj, std::align_val_t{alignof(ObjectHeader)});
    }

    template <typename Visitor>
    void trace_object_fields(ObjectHeader* obj, Visitor visit) {
        // Dispatch based on runtime type descriptor to scan references contained within heap objects.
        switch (obj->type) {
            case ObjectType::Array: {
                // Assuming array payload contains references to other ObjectHeaders
                // Payload structure layout handled dynamically by specific runtime classes.
                break;
            }
            case ObjectType::Map: {
                break;
            }
            case ObjectType::Closure: {
                break;
            }
            case ObjectType::String:
            case ObjectType::UserDefined:
            default:
                // Leaf objects containing no GC pointers.
                break;
        }
    }
};

} // namespace vector::vm

#endif // VECTOR_GC_HPP