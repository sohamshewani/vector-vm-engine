#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <cstdint>

// Dedicated Vector VM Engine Microbenchmark
// Implements a high-performance register-based vector VM abstraction,
// executing parallel SIMD-style arithmetic operations to measure throughput (ops/sec)
// and strict tail-latency percentiles (p50, p99).

namespace VectorVMEngine {

// VM Configuration
constexpr size_t VECTOR_WIDTH = 8;     // 8 elements per vector register
constexpr size_t NUM_REGISTERS = 16;
constexpr size_t MEMORY_SIZE = 65536;

enum class Opcode : uint8_t {
    VADD = 0,
    VMUL,
    VMA,    // Vector Multiply-Accumulate
    VREDUCE // Horizontal sum reduction
};

struct Instruction {
    Opcode op;
    uint8_t dst;
    uint8_t src1;
    uint8_t src2;
};

class VirtualMachine {
private:
    alignas(64) float registers[NUM_REGISTERS][VECTOR_WIDTH];
    alignas(64) float memory[MEMORY_SIZE];

public:
    VirtualMachine() {
        std::fill(&registers[0][0], &registers[0][0] + (NUM_REGISTERS * VECTOR_WIDTH), 1.0f);
        std::fill(memory, memory + MEMORY_SIZE, 0.5f);
    }

    // Execute a single vectorized VM instruction
    inline void execute(const Instruction& inst) noexcept {
        float* d = registers[inst.dst];
        const float* s1 = registers[inst.src1];
        const float* s2 = registers[inst.src2];

        switch (inst.op) {
            case Opcode::VADD:
                #pragma GCC ivdep
                for (size_t i = 0; i < VECTOR_WIDTH; ++i) {
                    d[i] = s1[i] + s2[i];
                }
                break;
            case Opcode::VMUL:
                #pragma GCC ivdep
                for (size_t i = 0; i < VECTOR_WIDTH; ++i) {
                    d[i] = s1[i] * s2[i];
                }
                break;
            case Opcode::VMA:
                #pragma GCC ivdep
                for (size_t i = 0; i < VECTOR_WIDTH; ++i) {
                    d[i] += s1[i] * s2[i];
                }
                break;
            case Opcode::VREDUCE: {
                float acc = 0.0f;
                #pragma GCC ivdep
                for (size_t i = 0; i < VECTOR_WIDTH; ++i) {
                    acc += s1[i];
                }
                d[0] = acc;
                break;
            }
        }
    }
};

} // namespace VectorVMEngine

int main() {
    using namespace VectorVMEngine;

    VirtualMachine vm;
    Instruction sample_program[] = {
        { Opcode::VADD,    1, 0, 0 },
        { Opcode::VMUL,    2, 1, 0 },
        { Opcode::VMA,     3, 2, 1 },
        { Opcode::VREDUCE, 0, 3, 0 }
    };
    constexpr size_t program_length = sizeof(sample_program) / sizeof(sample_program[0]);

    // Benchmark Parameters
    constexpr uint64_t WARMUP_ITERATIONS = 50000;
    constexpr uint64_t MEASUREMENT_ITERATIONS = 1000000;

    // Warmup phase
    for (uint64_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        vm.execute(sample_program[i % program_length]);
    }

    // Latency tracking array
    std::vector<uint64_t> latencies;
    latencies.reserve(MEASUREMENT_ITERATIONS);

    // High-resolution timing loop
    auto benchmark_start = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < MEASUREMENT_ITERATIONS; ++i) {
        const auto& inst = sample_program[i % program_length];
        
        auto start_ts = std::chrono::high_resolution_clock::now();
        vm.execute(inst);
        auto end_ts = std::chrono::high_resolution_clock::now();

        uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ts - start_ts).count();
        latencies.push_back(ns);
    }

    auto benchmark_end = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(benchmark_end - benchmark_start).count();

    // Compute metrics
    double ops_per_sec = static_cast<double>(MEASUREMENT_ITERATIONS) / total_seconds;

    std::sort(latencies.begin(), latencies.end());
    uint64_t p50 = latencies[MEASUREMENT_ITERATIONS * 50 / 100];
    uint64_t p99 = latencies[MEASUREMENT_ITERATIONS * 99 / 100];
    uint64_t p999 = latencies[Measurement_idx = MEASUREMENT_ITERATIONS * 999 / 1000];

    // Format output tabular metrics to stdout
    std::cout << "\n";
    std::cout << "===================================================================\n";
    std::cout << "               VECTOR-VM-ENGINE MICROBENCHMARK RESULTS             \n";
    std::cout << "===================================================================\n";
    std::cout << std::left << std::setw(30) << "Metric" 
              << std::right << std::setw(20) << "Value" 
              << std::right << std::setw(15) << "Unit" << "\n";
    std::cout << "-------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(30) << "Total Operations" 
              << std::right << std::setw(20) << MEASUREMENT_ITERATIONS 
              << std::right << std::setw(15) << "ops" << "\n";
    std::cout << std::left << std::setw(30) << "Execution Time" 
              << std::right << std::setw(20) << std::fixed << std::setprecision(4) << total_seconds 
              << std::right << std::setw(15) << "s" << "\n";
    std::cout << std::left << std::setw(30) << "Throughput" 
              << std::right << std::setw(20) << static_cast<uint64_t>(ops_per_sec) 
              << std::right << std::setw(15) << "ops/sec" << "\n";
    std::cout << std::left << std::setw(30) << "Latency p50" 
              << std::right << std::setw(20) << p50 
              << std::right << std::setw(15) << "ns" << "\n";
    std::cout << std::left << std::setw(30) << "Latency p99" 
              << std::right << std::setw(20) << p99 
              << std::right << std::setw(15) << "ns" << "\n";
    std::cout << std::left << std::setw(30) << "Latency p99.9" 
              << std::right << std::setw(20) << p999 
              << std::right << std::setw(15) << "ns" << "\n";
    std::cout << "===================================================================\n";
    std::cout << "\n";

    return 0;
}