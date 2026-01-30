# Default recipe - show available commands
default:
    @just --list

# Initialize/update git submodules
pull:
    git submodule init
    git submodule update --recursive --remote

# Format source code
format:
    make format

# Check formatting without fixing
format-check:
    make format-check

# Clean all build artifacts
clean:
    rm -rf build
    make clean

# Build debug with Ninja
config:
    GEN=ninja make debug

build:
    cmake --build build/debug --config Debug

# Run duckdb
run:
    ./build/debug/duckdb

# Run tests on debug build
test:
    ./build/debug/test/unittest "test/sql/quack.test"

spike:
    ./build/debug/test/unittest "test/sql/rbac/00_spikes.test"

# Update submodules to latest
update:
    make update

# Run clang-tidy checks
tidy:
    make tidy-check

