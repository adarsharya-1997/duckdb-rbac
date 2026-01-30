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
    # Phase 1 - Foundation
    ./build/debug/test/unittest "test/sql/rbac/00*"

    # Phase 2 - DDL Parsing
    ./build/debug/test/unittest "test/sql/rbac/01*"

    # Phase 3 - Storage & Grants
    ./build/debug/test/unittest "test/sql/rbac/02*"
    ./build/debug/test/unittest "test/sql/rbac/03*"
    ./build/debug/test/unittest "test/sql/rbac/06*"

    # Phase 4 - Enforcement
    ./build/debug/test/unittest "test/sql/rbac/05*"

    # Phase 5 - Row Policies
    ./build/debug/test/unittest "test/sql/rbac/04*"

    # Test Phase 6 files
    ./build/debug/test/unittest "test/sql/rbac/07*"
    ./build/debug/test/unittest "test/sql/rbac/08*"
    ./build/debug/test/unittest "test/sql/rbac/09*"

spike:
    ./build/debug/test/unittest "test/sql/rbac/00_spikes.test"

# Update submodules to latest
update:
    make update

# Run clang-tidy checks
tidy:
    make tidy-check

