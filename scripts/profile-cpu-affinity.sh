#!/bin/bash

# Flox Engine - CPU Affinity Performance Profiling Script
# Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
# 
# Copyright (c) 2025 FLOX Foundation
# Licensed under the MIT License. See LICENSE file in the project root for full
# license information.

set -e

# Configuration
BENCHMARK_BINARY="./build/benchmarks/cpu_affinity_benchmark"
RESULTS_DIR="./profiling_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
ITERATIONS=10

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}================================================${NC}"
    echo -e "${BLUE}  FLOX CPU Affinity Performance Profiling${NC}"
    echo -e "${BLUE}================================================${NC}"
    echo
    echo -e "${YELLOW}WARNING: This script should be run on an isolated machine${NC}"
    echo -e "${YELLOW}with minimal background processes. CPU affinity benchmarks${NC}"
    echo -e "${YELLOW}can show misleading results on busy/shared systems where${NC}"
    echo -e "${YELLOW}affinity may actually decrease performance.${NC}"
    echo
}

print_section() {
    echo -e "${GREEN}>>> $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}WARNING: $1${NC}"
}

print_error() {
    echo -e "${RED}ERROR: $1${NC}"
}

check_requirements() {
    print_section "Checking requirements..."
    
    # Check if perf is available
    if ! command -v perf &> /dev/null; then
        print_error "perf is required but not installed. Please install linux-perf package."
        exit 1
    fi
    
    # Check if benchmark binary exists
    if [[ ! -f "$BENCHMARK_BINARY" ]]; then
        print_error "Benchmark binary not found at $BENCHMARK_BINARY"
        echo "Please build the project with benchmarks enabled:"
        echo "  mkdir -p build && cd build"
        echo "  cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_BENCHMARK=ON"
        echo "  make -j\$(nproc)"
        exit 1
    fi
    
    # Check if running as root (for some perf features)
    if [[ $EUID -ne 0 ]]; then
        print_warning "Not running as root. Some perf features may be limited."
        echo "For full profiling capabilities, run with sudo."
    fi
    
    # Create results directory
    mkdir -p "$RESULTS_DIR"
    
    echo "✓ Requirements check passed"
    echo
}

get_system_info() {
    print_section "System Information"
    
    echo "CPU Information:"
    lscpu | grep -E "(Model name|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU\(s\))"
    
    echo
    echo "Memory Information:"
    free -h
    
    echo
    echo "Isolated CPU cores:"
    if [[ -f /sys/devices/system/cpu/isolated ]]; then
        cat /sys/devices/system/cpu/isolated
    else
        echo "No isolated cores configured"
    fi
    
    echo
    echo "CPU Governor:"
    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [[ -f "$cpu" ]]; then
            echo "$(basename $(dirname $cpu)): $(cat $cpu)"
            break
        fi
    done
    
    echo
}

run_basic_benchmarks() {
    print_section "Running Basic CPU Affinity Benchmarks"
    
    local output_file="$RESULTS_DIR/basic_benchmarks_${TIMESTAMP}.txt"
    
    echo "Running benchmarks (this may take a few minutes)..."
    "$BENCHMARK_BINARY" --benchmark_format=console --benchmark_out="$output_file" --benchmark_out_format=json
    
    echo "Basic benchmark results saved to: $output_file"
    echo
}

run_perf_stat_comparison() {
    print_section "Running perf stat comparison"
    
    local perf_events="cycles,instructions,cache-references,cache-misses,branches,branch-misses,context-switches,cpu-migrations,page-faults,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses"
    
    # Run with affinity
    print_section "Profiling WITH CPU affinity..."
    local with_affinity_file="$RESULTS_DIR/perf_with_affinity_${TIMESTAMP}.txt"
    perf stat -e "$perf_events" -r $ITERATIONS --table -o "$with_affinity_file" \
        "$BENCHMARK_BINARY" --benchmark_filter=".*WithAffinity.*" --benchmark_min_time=1.0 \
        > /dev/null 2>&1 || true
    
    # Run without affinity
    print_section "Profiling WITHOUT CPU affinity..."
    local without_affinity_file="$RESULTS_DIR/perf_without_affinity_${TIMESTAMP}.txt"
    perf stat -e "$perf_events" -r $ITERATIONS --table -o "$without_affinity_file" \
        "$BENCHMARK_BINARY" --benchmark_filter=".*WithoutAffinity.*" --benchmark_min_time=1.0 \
        > /dev/null 2>&1 || true
    
    echo "Perf stat results saved to:"
    echo "  With affinity: $with_affinity_file"
    echo "  Without affinity: $without_affinity_file"
    echo
}

run_perf_record_analysis() {
    print_section "Running detailed perf record analysis"
    
    if [[ $EUID -ne 0 ]]; then
        print_warning "Skipping perf record analysis (requires root privileges)"
        return
    fi
    
    local perf_data_with="$RESULTS_DIR/perf_with_affinity_${TIMESTAMP}.data"
    local perf_data_without="$RESULTS_DIR/perf_without_affinity_${TIMESTAMP}.data"
    
    # Record with affinity
    print_section "Recording WITH CPU affinity..."
    perf record -g -o "$perf_data_with" \
        "$BENCHMARK_BINARY" --benchmark_filter=".*WithAffinity.*" --benchmark_min_time=2.0 \
        > /dev/null 2>&1 || true
    
    # Record without affinity
    print_section "Recording WITHOUT CPU affinity..."
    perf record -g -o "$perf_data_without" \
        "$BENCHMARK_BINARY" --benchmark_filter=".*WithoutAffinity.*" --benchmark_min_time=2.0 \
        > /dev/null 2>&1 || true
    
    # Generate reports
    if [[ -f "$perf_data_with" ]]; then
        perf report -i "$perf_data_with" --stdio > "$RESULTS_DIR/perf_report_with_affinity_${TIMESTAMP}.txt"
    fi
    
    if [[ -f "$perf_data_without" ]]; then
        perf report -i "$perf_data_without" --stdio > "$RESULTS_DIR/perf_report_without_affinity_${TIMESTAMP}.txt"
    fi
    
    echo "Perf record analysis completed"
    echo
}

analyze_cache_performance() {
    print_section "Analyzing cache performance"
    
    local cache_events="L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores,L1-dcache-store-misses,L1-icache-loads,L1-icache-load-misses,LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses"
    
    # Test memory access patterns
    print_section "Testing memory access patterns..."
    
    # With affinity
    perf stat -e "$cache_events" -r 5 \
        "$BENCHMARK_BINARY" --benchmark_filter="BM_MemoryAccess_WithAffinity" \
        > "$RESULTS_DIR/cache_with_affinity_${TIMESTAMP}.txt" 2>&1 || true
    
    # Without affinity
    perf stat -e "$cache_events" -r 5 \
        "$BENCHMARK_BINARY" --benchmark_filter="BM_MemoryAccess_WithoutAffinity" \
        > "$RESULTS_DIR/cache_without_affinity_${TIMESTAMP}.txt" 2>&1 || true
    
    echo "Cache performance analysis completed"
    echo
}

run_latency_distribution_analysis() {
    print_section "Running latency distribution analysis"
    
    # Create a simple latency measurement tool
    cat > "$RESULTS_DIR/latency_test_${TIMESTAMP}.cpp" << 'EOF'
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <sched.h>

void pinToCore(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

int main(int argc, char* argv[]) {
    const int iterations = 100000;
    const bool useAffinity = (argc > 1 && std::string(argv[1]) == "with_affinity");
    
    if (useAffinity) {
        pinToCore(0);
    }
    
    std::vector<uint64_t> latencies;
    latencies.reserve(iterations);
    
    for (int i = 0; i < iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Simulate work
        volatile int x = 0;
        for (int j = 0; j < 1000; ++j) {
            x += j;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        latencies.push_back(latency);
    }
    
    std::sort(latencies.begin(), latencies.end());
    
    auto percentile = [&](double p) {
        return latencies[static_cast<size_t>(p * latencies.size() / 100.0)];
    };
    
    std::cout << "Latency Distribution (" << (useAffinity ? "with" : "without") << " affinity):" << std::endl;
    std::cout << "Min: " << latencies.front() << " ns" << std::endl;
    std::cout << "P50: " << percentile(50) << " ns" << std::endl;
    std::cout << "P95: " << percentile(95) << " ns" << std::endl;
    std::cout << "P99: " << percentile(99) << " ns" << std::endl;
    std::cout << "P99.9: " << percentile(99.9) << " ns" << std::endl;
    std::cout << "Max: " << latencies.back() << " ns" << std::endl;
    
    return 0;
}
EOF

    # Compile and run latency test
    g++ -O3 -std=c++17 -o "$RESULTS_DIR/latency_test_${TIMESTAMP}" "$RESULTS_DIR/latency_test_${TIMESTAMP}.cpp"
    
    echo "Latency distribution with affinity:"
    "$RESULTS_DIR/latency_test_${TIMESTAMP}" with_affinity > "$RESULTS_DIR/latency_with_affinity_${TIMESTAMP}.txt"
    cat "$RESULTS_DIR/latency_with_affinity_${TIMESTAMP}.txt"
    
    echo
    echo "Latency distribution without affinity:"
    "$RESULTS_DIR/latency_test_${TIMESTAMP}" without_affinity > "$RESULTS_DIR/latency_without_affinity_${TIMESTAMP}.txt"
    cat "$RESULTS_DIR/latency_without_affinity_${TIMESTAMP}.txt"
    
    echo
}

generate_summary_report() {
    print_section "Generating summary report"
    
    local summary_file="$RESULTS_DIR/summary_report_${TIMESTAMP}.txt"
    
    cat > "$summary_file" << EOF
FLOX CPU Affinity Performance Profiling Summary
==============================================
Timestamp: $TIMESTAMP
System: $(uname -a)

CPU Information:
$(lscpu | grep -E "(Model name|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU\(s\))")

Memory Information:
$(free -h)

Isolated CPU cores:
$(if [[ -f /sys/devices/system/cpu/isolated ]]; then cat /sys/devices/system/cpu/isolated; else echo "No isolated cores configured"; fi)

Files Generated:
EOF
    
    # List all generated files
    ls -la "$RESULTS_DIR"/*"$TIMESTAMP"* >> "$summary_file"
    
    echo "Summary report generated: $summary_file"
    echo
}

main() {
    print_header
    
    check_requirements
    get_system_info
    run_basic_benchmarks
    run_perf_stat_comparison
    run_perf_record_analysis
    analyze_cache_performance
    run_latency_distribution_analysis
    generate_summary_report
    
    print_section "Performance profiling completed!"
    echo "Results are available in: $RESULTS_DIR"
    echo
    echo "Key findings to look for:"
    echo "1. Reduced cache misses with CPU affinity"
    echo "2. Fewer context switches and CPU migrations"
    echo "3. More consistent latency distribution"
    echo "4. Better branch prediction performance"
    echo "5. Higher instructions per cycle (IPC)"
    echo
    echo "To analyze results further:"
    echo "  - Compare perf stat outputs for context switches and cache misses"
    echo "  - Check latency distribution percentiles for consistency"
    echo "  - Look for reduced CPU migrations in perf reports"
    echo
}

# Handle script arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [OPTIONS]"
        echo "Options:"
        echo "  --help, -h     Show this help message"
        echo "  --quick        Run quick profiling (fewer iterations)"
        echo "  --detailed     Run detailed profiling (more iterations)"
        exit 0
        ;;
    --quick)
        ITERATIONS=3
        ;;
    --detailed)
        ITERATIONS=20
        ;;
esac

main "$@" 