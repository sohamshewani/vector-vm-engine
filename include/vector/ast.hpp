#ifndef VECTOR_VM_AST_HPP
#define VECTOR_VM_AST_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string_view>
#include <string>
#include <memory>
#include <variant>
#include <type_traits>
#include <new>
#include <utility>
#include <system_error>

namespace vector::vm {

enum class NodeType : uint8_t {
    LiteralInt,
    LiteralFloat,
    LiteralString,
    LiteralBool,
    Identifier,
    BinaryOp,
    UnaryOp,
    Assign,
    Call,
    If,
    While,
    Return,
    Block,
    FunctionDecl
};

enum class BinaryOperator : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    NotEq,
    Lt,
    LtEq,
    Gt,
    GtEq,
    LogicalAnd,
    LogicalOr
};

enum class UnaryOperator : uint8_t {
    Negate,
    LogicalNot,
    BitwiseNot
};

struct SourceLocation {
    uint32_t line;
    uint32_t column;
    uint32_t file_id;
};

class ArenaPage {
public:
    static constexpr size_t kPageSize = 2 * 1024 * 1024; // 2MB Huge Page

    ArenaPage() : capacity_(kPageSize - sizeof(void*) - sizeof(size_t)), used_(0) {
        data_ = static_cast<char*>(::operator new(kPageSize, std::align_val_t{64}));
        next_ = nullptr;
    }

    ~ArenaPage() {
        if (data_) {
            ::operator delete(data_, std::align_val_t{64});
        }
    }

    ArenaPage(const ArenaPage&) = delete;
    ArenaPage& operator=(const ArenaPage&) = delete;
    ArenaPage(ArenaPage&& other) noexcept 
        : data_(other.data_), capacity_(other.capacity_), used_(other.used_), next_(other.next_) {
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.used_ = 0;
        other.next_ = nullptr;
    }

    ArenaPage& operator=(ArenaPage&& other) noexcept {
        if (this != &other) {
            if (data_) {
                ::operator delete(data_, std::align_val_t{64});
            }
            data_ = other.data_;
            capacity_ = other.capacity_;
            used_ = other.used_;
            next_ = other.next_;
            other.data_ = nullptr;
            other.capacity_ = 0;
            other.used_ = 0;
            other.next_ = nullptr;
        }
        return resend();
    }

    [[nodiscard]] void* allocate(size_t size, size_t alignment) {
        uintptr_t current_address = reinterpret_cast<uintptr_t>(data_ + used_);
        size_t adjustment = (alignment - (current_address % alignment)) % alignment;
        size_t total_needed = size + adjustment;

        if (used_ + total_needed > capacity_) {
            return nullptr;
        }

        used_ += total_needed;
        return reinterpret_cast<void*>(current_address + adjustment);
    }

    [[nodiscard]] size_t available() const noexcept {
        return capacity_ - used_;
    }

    ArenaPage* next() const noexcept { return next_; }
    void set_next(ArenaPage* page) noexcept { next_ = page; }

private:
    ArenaPage& resend() noexcept { return *this; }

    char* data_;
    size_t capacity_;
    size_t used_;
    ArenaPage* next_;
};

class ArenaAllocator {
public:
    ArenaAllocator() : head_(nullptr), total_allocated_(0) {
        allocate_new_page();
    }

    ~ArenaAllocator() {
        ArenaPage* current = head_;
        while (current != nullptr) {
            ArenaPage* next = current->next();
            current->~ArenaPage();
            ::operator delete(current, std::align_val_t{alignof(ArenaPage)});
            current = next;
        }
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&& other) noexcept 
        : head_(other.head_), total_allocated_(other.total_allocated_) {
        other.head_ = nullptr;
        other.total_allocated_ = 0;
    }

    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept {
        if (this != &other) {
            ArenaPage* current = head_;
            while (current != nullptr) {
                ArenaPage* next = current->next();
                current->~ArenaPage();
                ::operator delete(current, std::align_val_t{alignof(ArenaPage)});
                current = next;
            }
            head_ = other.head_;
            total_allocated_ = other.total_allocated_;
            other.head_ = nullptr;
            other.total_allocated_ = 0;
        }
        return *this;
    }

    [[nodiscard]] void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        if (size == 0) {
            return nullptr;
        }

        void* ptr = head_->allocate(size, alignment);
        if (ptr == nullptr) {
            allocate_new_page();
            ptr = head_->allocate(size, alignment);
            if (ptr == nullptr) {
                throw std::bad_alloc();
            }
        }
        total_allocated_ += size;
        return ptr;
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    [[nodiscard]] size_t total_bytes_allocated() const noexcept {
        return total_allocated_;
    }

private:
    void allocate_new_page() {
        void* page_mem = ::operator new(sizeof(ArenaPage), std::align_val_t{alignof(ArenaPage)});
        ArenaPage* new_page = ::new (page_mem) ArenaPage();
        new_page->set_next(head_);
        head_ = new_page;
    }

    ArenaPage* head_;
    size_t total_allocated_;
};

template <typename T>
class ArenaVector {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = T*;
    using const_iterator = const T*;

    explicit ArenaVector(ArenaAllocator& arena) noexcept 
        : arena_(arena), data_(nullptr), size_(0), capacity_(0) {}

    ArenaVector(ArenaAllocator& arena, size_type count) 
        : arena_(arena), data_(nullptr), size_(0), capacity_(0) {
        reserve(count);
        for (size_type i = 0; i < count; ++i) {
            emplace_back();
        }
    }

    ArenaVector(ArenaAllocator& arena, size_type count, const T& value) 
        : arena_(arena), data_(nullptr), size_(0), capacity_(0) {
        reserve(count);
        for (size_type i = 0; i < count; ++i) {
            emplace_back(value);
        }
    }

    ~ArenaVector() {
        clear();
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
            clear();
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
        emplace_back(value);
    }

    void push_back(T&& value) {
        emplace_back(std::move(value));
    }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (size_ >= capacity_) {
            size_type new_capacity = (capacity_ == 0) ? 4 : capacity_ * 2;
            reserve(new_capacity);
        }
        pointer addr = data_ + size_;
        pointer obj = ::new (static_cast<void*>(addr)) T(std::forward<Args>(args)...);
        ++size_;
        return *obj;
    }

    void pop_back() noexcept {
        if (size_ > 0) {
            --size_;
            data_[size_].~T();
        }
    }

    void reserve(size_type new_capacity) {
        if (new_capacity <= capacity_) {
            return;
        }

        pointer new_data = static_cast<pointer>(arena_.allocate(new_capacity * sizeof(T), alignof(T)));
        if constexpr (std::is_move_constructible_v<T>) {
            for (size_type i = 0; i < size_; ++i) {
                ::new (static_cast<void*>(new_data + i)) T(std::move(data_[i]));
                data_[i].~T();
            }
        } else {
            for (size_type i = 0; i < size_; ++i) {
                ::new (static_cast<void*>(new_data + i)) T(data_[i]);
                data_[i].~T();
            }
        }

        data_ = new_data;
        capacity_ = new_capacity;
    }

    void clear() noexcept {
        for (size_type i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = 0;
    }

    [[nodiscard]] reference operator[](size_type pos) noexcept {
        return data_[pos];
    }

    [[nodiscard]] const_reference operator[](size_type pos) const noexcept {
        return data_[pos];
    }

    [[nodiscard]] reference at(size_type pos) {
        if (pos >= size_) {
            throw std::out_of_range("ArenaVector index out of range");
        }
        return data_[pos];
    }

    [[nodiscard]] const_reference at(size_type pos) const {
        if (pos >= size_) {
            throw std::out_of_range("ArenaVector index out of range");
        }
        return data_[pos];
    }

    [[nodiscard]] reference front() noexcept { return data_[0]; }
    [[nodiscard]] const_reference front() const noexcept { return data_[0]; }
    [[nodiscard]] reference back() noexcept { return data_[size_ - 1]; }
    [[nodiscard]] const_reference back() const noexcept { return data_[size_ - 1]; }

    [[nodiscard]] pointer data() noexcept { return data_; }
    [[nodiscard]] const_pointer data() const noexcept { return data_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type capacity() const noexcept { return capacity_; }

    [[nodiscard]] iterator begin() noexcept { return data_; }
    [[nodiscard]] const_iterator begin() const noexcept { return data_; }
    [[nodiscard]] const_iterator cbegin() const noexcept { return data_; }
    [[nodiscard]] iterator end() noexcept { return data_ + size_; }
    [[nodiscard]] const_iterator end() const noexcept { return data_ + size_; }
    [[nodiscard]] const_iterator cend() const noexcept { return data_ + size_; }

private:
    ArenaAllocator& arena_;
    pointer data_;
    size_type size_;
    size_type capacity_;
};

class ASTNode {
public:
    explicit ASTNode(NodeType type, SourceLocation loc) noexcept 
        : type_(type), location_(loc) {}

    virtual ~ASTNode() = default;

    ASTNode(const ASTNode&) = delete;
    ASTNode& operator=(const ASTNode&) = delete;
    ASTNode(ASTNode&&) noexcept = default;
    ASTNode& operator=(ASTNode&&) noexcept = default;

    [[nodiscard]] NodeType type() const noexcept { return type_; }
    [[nodiscard]] SourceLocation location() const noexcept { return location_; }

private:
    NodeType type_;
    SourceLocation location_;
};

class LiteralIntNode final : public ASTNode {
public:
    LiteralIntNode(int64_t value, SourceLocation loc) noexcept 
        : ASTNode(NodeType::LiteralInt, loc), value_(value) {}

    [[nodiscard]] int64_t value() const noexcept { return value_; }

private:
    int64_t value_;
};

class LiteralFloatNode final : public ASTNode {
public:
    LiteralFloatNode(double value, SourceLocation loc) noexcept 
        : ASTNode(NodeType::LiteralFloat, loc), value_(value) {}

    [[nodiscard]] double value() const noexcept { return value_; }

private:
    double value_;
};

class LiteralStringNode final : public ASTNode {
public:
    LiteralStringNode(std::string_view value, SourceLocation loc) noexcept 
        : ASTNode(NodeType::LiteralString, loc), value_(value) {}

    [[nodiscard]] std::string_view value() const noexcept { return value_; }

private:
    std::string_view value_;
};

class LiteralBoolNode final : public ASTNode {
public:
    LiteralBoolNode(bool value, SourceLocation loc) noexcept 
        : ASTNode(NodeType::LiteralBool, loc), value_(value) {}

    [[nodiscard]] bool value() const noexcept { return value_; }

private:
    bool value_;
};

class IdentifierNode final : public ASTNode {
public:
    IdentifierNode(std::string_view name, SourceLocation loc) noexcept 
        : ASTNode(NodeType::Identifier, loc), name_(name) {}

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    std::string_view name_;
};

class BinaryOpNode final : public ASTNode {
public:
    BinaryOpNode(BinaryOperator op, ASTNode* left, ASTNode* right, SourceLocation loc) noexcept 
        : ASTNode(NodeType::BinaryOp, loc), op_(op), left_(left), right_(right) {}

    [[nodiscard]] BinaryOperator op() const noexcept { return op_; }
    [[nodiscard]] ASTNode* left() const noexcept { return left_; }
    [[nodiscard]] ASTNode* right() const noexcept { return right_; }

private:
    BinaryOperator op_;
    ASTNode* left_;
    ASTNode* right_;
};

class UnaryOpNode final : public ASTNode {
public:
    UnaryOpNode(UnaryOperator op, ASTNode* operand, SourceLocation loc) noexcept 
        : ASTNode(NodeType::UnaryOp, loc), op_(op), operand_(operand) {}

    [[nodiscard]] UnaryOperator op() const noexcept { return op_; }
    [[nodiscard]] ASTNode* operand() const noexcept { return operand_; }

private:
    UnaryOperator op_;
    ASTNode* operand_;
};

class AssignNode final : public ASTNode {
public:
    AssignNode(std::string_view name, ASTNode* value, Source