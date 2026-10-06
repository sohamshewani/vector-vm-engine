#include <iostream>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <random>
#include <chrono>
#include <stdexcept>
#include <mutex>

// ==========================================
// VMX ENGINE SPECIFICATION (Embedded for Testing)
// ==========================================

namespace vmx {

enum class Opcode : uint8_t {
    NOP = 0x00,
    ADD = 0x01,
    SUB = 0x02,
    MUL = 0x03,
    DIV = 0x04,
    MOV = 0x05,
    JMP = 0x06,
    JZ  = 0x07,
    JNZ = 0x08,
    HLT = 0xFF
};

struct Instruction {
    Opcode op;
    uint8_t reg_dest;
    uint8_t reg_src1;
    uint8_t reg_src2;
    int32_t imm;
};

class VMException : public std::runtime_error {
public:
    explicit VMException(const std::string& msg) : std::runtime_error(msg) {}
};

class VirtualMachine {
public:
    static constexpr size_t REG_COUNT = 16;
    static constexpr size_t MAX_MEMORY = 1024 * 1024; // 1MB

    VirtualMachine() {
        reset();
    }

    ~VirtualMachine() = default;

    void reset() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::fill(m_regs.begin(), m_regs.end(), 0);
        m_pc = 0;
        m_flags_zero = false;
        m_halted = false;
        m_faulted = false;
        m_fuel = 0;
        m_memory.clear();
        m_memory.shrink_to_fit();
        m_memory.resize(4096, 0); // Default 4KB initial memory
    }

    void load_code(const std::vector<Instruction>& code) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_code = code;
        m_pc = 0;
        m_halted = false;
        m_faulted = false;
    }

    void set_fuel(uint64_t fuel) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_fuel = fuel;
    }

    uint64_t get_fuel() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_fuel;
    }

    int32_t get_register(size_t reg) const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (reg >= REG_COUNT) throw VMException("Invalid register index");
        return m_regs[reg];
    }

    void set_register(size_t reg, int32_t val) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (reg >= REG_COUNT) throw VMException("Invalid register index");
        m_regs[reg] = val;
    }

    size_t get_pc() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_pc;
    }

    bool is_halted() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_halted;
    }

    bool is_faulted() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_faulted;
    }

    void step() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_halted) throw VMException("Execution halted");
        if (m_faulted) throw VMException("VM in faulted state");
        if (m_fuel == 0 && m_fuel_enabled) {
            m_faulted = true;
            throw VMException("Out of fuel");
        }
        if (m_pc >= m_code.size()) {
            m_halted = true;
            return;
        }

        if (m_fuel_enabled) {
            m_fuel--;
        }

        const Instruction& inst = m_code[m_pc++];
        execute_instruction(inst);
    }

    void run(uint64_t max_steps = 1000000) {
        uint64_t steps = 0;
        while (!is_halted() && !is_faulted() && steps < max_steps) {
            step();
            steps++;
        }
        if (steps >= max_steps && !is_halted()) {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_faulted = true;
            throw VMException("Execution timeout / max steps reached");
        }
    }

    void enable_fuel(bool enable) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_fuel_enabled = enable;
    }

    // Memory operations
    void write_mem(size_t addr, uint8_t val) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (addr >= MAX_MEMORY) {
            m_faulted = true;
            throw VMException("Segmentation fault: write out of bounds");
        }
        if (addr >= m_memory.size()) {
            m_memory.resize(std::max(addr + 1, m_memory.size() * 2), 0);
        }
        m_memory[addr] = val;
    }

    uint8_t read_mem(size_t addr) const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (addr >= MAX_MEMORY || addr >= m_memory.size()) {
            throw VMException("Segmentation fault: read out of bounds");
        }
        return m_memory[addr];
    }

private:
    void execute_instruction(const Instruction& inst) {
        // Validate register bounds
        if (inst.reg_dest >= REG_COUNT || inst.reg_src1 >= REG_COUNT || inst.reg_src2 >= REG_COUNT) {
            m_faulted = true;
            throw VMException("Invalid register encoding in instruction");
        }

        switch (inst.op) {
            case Opcode::NOP:
                break;
            case Opcode::ADD:
                m_regs[inst.reg_dest] = m_regs[inst.reg_src1] + m_regs[inst.reg_src2];
                m_flags_zero = (m_regs[inst.reg_dest] == 0);
                break;
            case Opcode::SUB:
                m_regs[inst.reg_dest] = m_regs[inst.reg_src1] - m_regs[inst.reg_src2];
                m_flags_zero = (m_regs[inst.reg_dest] == 0);
                break;
            case Opcode::MUL:
                m_regs[inst.reg_dest] = m_regs[inst.reg_src1] * m_regs[inst.reg_src2];
                m_flags_zero = (m_regs[inst.reg_dest] == 0);
                break;
            case Opcode::DIV:
                if (m_regs[inst.reg_src2] == 0) {
                    m_faulted = true;
                    throw VMException("Division by zero");
                }
                m_regs[inst.reg_dest] = m_regs[inst.reg_src1] / m_regs[inst.reg_src2];
                m_flags_zero = (m_regs[inst.reg_dest] == 0);
                break;
            case Opcode::MOV:
                m_regs[inst.reg_dest] = inst.imm;
                m_flags_zero = (m_regs[inst.reg_dest] == 0);
                break;
            case Opcode::JMP:
                if (inst.imm < 0 || static_cast<size_t>(inst.imm) > m_code.size()) {
                    m_faulted = true;
                    throw VMException("Invalid jump target");
                }
                m_pc = static_cast<size_t>(inst.imm);
                break;
            case Opcode::JZ:
                if (m_flags_zero) {
                    if (inst.imm < 0 || static_cast<size_t>(inst.imm) > m_code.size()) {
                        m_faulted = true;
                        throw VMException("Invalid jump target");
                    }
                    m_pc = static_cast<size_t>(inst.imm);
                }
                break;
            case Opcode::JNZ:
                if (!m_flags_zero) {
                    if (inst.imm < 0 || static_cast<size_t>(inst.imm) > m_code.size()) {
                        m_faulted = true;
                        throw VMException("Invalid jump target");
                    }
                    m_pc = static_cast<size_t>(inst.imm);
                }
                break;
            case Opcode::HLT:
                m_halted = true;
                break;
            default:
                m_faulted = true;
                throw VMException("Unknown opcode");
        }
    }

    mutable std::recursive_mutex m_mutex;
    std::vector<int32_t> m_regs{std::vector<int32_t>(REG_COUNT, 0)};
    size_t m_pc{0};
    bool m_flags_zero{false};
    bool m_halted{false};
    bool m_faulted{false};
    uint64_t m_fuel{0};
    bool m_fuel_enabled{false};
    std::vector<Instruction> m_code;
    std::vector<uint8_t> m_memory;
};

} // namespace vmx

// ==========================================
// COMPREHENSIVE TEST SUITE
// ==========================================

class TestSuite {
private:
    int m_passed = 0;
    int m_failed = 0;

    void assert_true(bool condition, const std::string& test_name) {
        if (condition) {
            m_passed++;
            std::cout << "[PASS] " << test_name << "\n";
        } else {
            m_failed++;
            std::cerr << "[FAIL] " << test_name << "\n";
        }
    }

public:
    void run_all() {
        std::cout << "==========================================\n";
        std::cout << " STARTING VMX ENGINE TEST SUITE\n";
        std::cout << "==========================================\n";

        // Unit Tests
        test_arithmetic_operations();
        test_control_flow();
        test_boundary_conditions();

        // Invariant & Edge-Case Assertions
        test_fuel_exhaustion();
        test_memory_safety();
        test_fault_recovery();

        // Concurrent & Stress Testing
        test_concurrent_contention();
        test_stress_randomized_execution();

        std::cout << "==========================================\n";
        std::cout << " TEST RESULTS:\n";
        std::cout << " PASSED: " << m_passed << "\n";
        std::cout << " FAILED: " << m_failed << "\n";
        std::cout << "==========================================\n";
    }

    int get_fail_count() const { return m_failed; }

private:
    void test_arithmetic_operations() {
        vmx::VirtualMachine vm;
        std::vector<vmx::Instruction> code = {
            {vmx::Opcode::MOV, 1, 0, 0, 10}, // R1 = 10
            {vmx::Opcode::MOV, 2, 0, 0, 20}, // R2 = 20
            {vmx::Opcode::ADD, 3, 1, 2, 0},  // R3 = R1 +