# vector-vm-engine
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) [![CI](https://github.com/sohamshewani/vector-vm-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/sohamshewani/vector-vm-engine/actions) [![Security: Hardened](https://img.shields.io/badge/Security-Hardened-blue.svg)](https://github.com/sohamshewani/vector-vm-engine) [![Stars](https://img.shields.io/github/stars/sohamshewani/vector-vm-engine?style=flat)](https://github.com/sohamshewani/vector-vm-engine)

## Executive Summary & Problem Space

`vector-vm-engine` is a high-throughput, low-latency execution engine and register-based Virtual Machine designed for vectorized workloads, graph computations, and real-time state transitions. In modern distributed systems and high-frequency analytical pipelines, traditional interpreter-based engines suffer from massive instruction-cache misses, unpredictable heap allocation overhead, and severe lock contention during multi-threaded garbage collection phases. 

`vector-vm-engine` solves these foundational bottlenecks through:
* **Zero-Copy Bytecode Compilation:** Transforming AST representations (`include/vector/ast.hpp`) into densely packed, cache-aligned instruction streams (`include/vector/compiler.hpp`).
* **Deterministic Garbage Collection:** A generational, mark-sweep tracing GC (`include/vector/gc.hpp`) engineered to bound pause times under strict sub-millisecond SLAs.
* **Lock-Free Pipeline Execution:** Utilizing hazard pointers and lock-free rings to decouple execution threads from state mutations (`src/engine.cpp`).

---

## Structural ASCII Architecture / Dataflow Diagram

The system is engineered as a decoupled, multi-stage pipeline maximizing CPU cache locality and hardware prefetching.

```
 +-----------------------------------------------------------------------+
 |                         Client / Host Application                     |
 +-----------------------------------------------------------------------+
                                    |
                                    v (Raw Source / Query)
 +-----------------------------------------------------------------------+
 |                     AST Parser & Semantic Analyzer                    |
 |                     (`include/vector/ast.hpp`)                        |
 +-----------------------------------------------------------------------+
                                    |
                                    v (Abstract Syntax Tree)
 +-----------------------------------------------------------------------+
 |                     Optimizing Bytecode Compiler                      |
 |                     (`include/vector/compiler.hpp`)                   |
 +-----------------------------------------------------------------------+
                                    |
                                    v (Dense Instruction Stream)
 +-----------------------------------------------------------------------+
 |                      Register-Based Virtual Machine                   |
 |                      (`include/vector/vm.hpp`)                        |
 |                                                                       |
 |   +---------------------------------------------------------------+   |
 |   |              Execution Context & Register File                |   |
 |   +---------------------------------------------------------------+   |
 |   |          Hazard Pointer / Thread-Local State Registry         |   |
 |   +---------------------------------------------------------------+   |
 +-----------------------------------------------------------------------+
            |                                           |
            | (Allocation / Reference)                  | (State Mutation)
            v                                           v
 +-----------------------------+             +---------------------------+
 |   Generational GC Subsystem |             |       Engine Core         |
 |   (`include/vector/gc.hpp`) |<----------->| (`src/engine.cpp`)        |
 +-----------------------------+             +---------------------------+
```

---

## Concurrency Model & Memory Invariants

`vector-vm-engine` adheres to rigorous memory management contracts to ensure thread safety without sacrificing execution speed:

* **Thread Safety & Lock Contention Prevention:** VM execution contexts are strictly thread-local. Inter-thread communication relies on lock-free message queues and atomic state swapping, eliminating mutex acquisition bottlenecks on the hot execution path.
* **Memory Ownership & Lifetimes (`include/vector/types.hpp`):** Raw pointers across VM boundaries are forbidden. All heap-allocated resources are governed by custom smart pointer wrappers that integrate directly with the tracing garbage collector.
* **Atomic Primitives:** Synchronization barriers use acquire-release semantics (`std::memory_order_acquire` / `std::memory_order_release`) to guarantee visibility of mutable bytecode states across execution cores without incurring full sequential consistency penalties.
* **Invariants Verification:** Continuously validated via comprehensive invariant test suites (`tests/test_invariants.cpp`).

---

## Performance Characteristics & Algorithmic Complexity Table

| Operation | Target Subsystem | Time Complexity | Space Complexity | Latency SLA (p99) |
| :--- | :--- | :--- | :--- | :--- |
| **AST Compilation** | `compiler.hpp` | $\mathcal{O}(N)$ | $\mathcal{O}(N)$ | $< 120\,\mu\text{s}$ |
| **Bytecode Execution** | `vm.hpp` | $\mathcal{O}(I)$ | $\mathcal{O}(R)$ | $< 15\,\text{ns / op}$ |
| **Garbage Collection (Minor)** | `gc.hpp` | $\mathcal{O}(L)$ | $\mathcal{O}(1)$ | $< 500\,\mu\text{s}$ |
| **Engine State Mutation** | `engine.cpp` | $\mathcal{O}(1)$ | $\mathcal{O}(1)$ | $< 45\,\text{ns}$ |

*Legend: $N$ = AST nodes, $I$ = Executed instructions, $R$ = Active registers, $L$ = Live heap objects.*

---

## Security Architecture & Threat Model

Designed for deployment in hostile multi-tenant environments, `vector-vm-engine` enforces strict safety boundaries:

* **Input Boundaries & Sanitization:** Bytecode binaries undergo rigorous structural and bounds verification during the compilation phase (`include/vector/compiler.hpp`). Malformed or out-of-bounds jump targets are rejected prior to execution.
* **Memory Corruption Protections:** Stack overflows, buffer over-reads, and use-after-free vectors are neutralized through strict type-safe abstractions (`include/vector/types.hpp`) and heap-isolation zones managed by the GC.
* **Sanitization Integration:** The codebase is fully compatible with Clang/GCC AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan), enforced continuously via CI pipelines.
* **Threat Vectors Mitigated:** Arbitrary Code Execution (ACE) via bytecode injection, memory exhaustion attacks through recursive object allocation, and dangling pointer exploitation.

---

## Build, Verification & Benchmarking Recipes

Ensure your build environment has a modern C++20 compliant compiler (`g++` >= 11 or `clang++` >= 13) and GNU Make installed.

### 1. Compile the Engine & Tests
```bash
# Build production binaries and test suites
make clean
make all
```

### 2. Run Invariant & Unit Tests
```bash
# Execute memory invariant and VM correctness tests
make test
```

### 3. Execute High-Performance Benchmarks
```bash
# Run the engine micro-benchmarks (`benchmarks/bench_engine.cpp`)
make benchmark