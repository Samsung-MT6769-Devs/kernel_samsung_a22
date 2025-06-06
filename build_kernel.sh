#!/bin/bash

# Color definitions for better readability
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Function to print colored status messages
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_section() {
    echo -e "\n${PURPLE}=== $1 ===${NC}"
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to display build summary
show_build_info() {
    local start_time=$1
    local end_time=$2
    local duration=$((end_time - start_time))
    local minutes=$((duration / 60))
    local seconds=$((duration % 60))

    print_section "BUILD SUMMARY"
    echo -e "  ${CYAN}Build Time:${NC} ${minutes}m ${seconds}s"
    echo -e "  ${CYAN}Git Commit:${NC} $(git rev-parse --short HEAD 2>/dev/null || echo 'N/A')"
    echo -e "  ${CYAN}Git Branch:${NC} $(git symbolic-ref --short HEAD 2>/dev/null || echo 'N/A')"
    echo -e "  ${CYAN}Kernel Image:${NC} $(ls -lh out/arch/arm64/boot/Image 2>/dev/null | awk '{print $5}' || echo 'Not found')"
}

# Function to create flashable zip
create_flashable_zip() {
    local source_zip="/home/zears/Documents/WMKernel-ksunext-susfs.zip"
    local anykernel_dir="$PREFIX/AnyKernel3"
    local kernel_image="$PREFIX/arch/arm64/boot/Image"

    print_section "FLASHABLE ZIP CREATION"

    # Check if source zip exists
    if [ ! -f "$source_zip" ]; then
        print_warning "Source zip not found: $source_zip"
        print_status "Skipping flashable zip creation"
        return 1
    fi

    # Check if kernel image exists
    if [ ! -f "$kernel_image" ]; then
        print_error "Kernel image not found: $kernel_image"
        return 1
    fi

    # Create AnyKernel3 directory if it doesn't exist
    if [ ! -d "$anykernel_dir" ]; then
        print_status "Creating AnyKernel3 directory..."
        mkdir -p "$anykernel_dir"
    fi

    # Get git information for filename
    local current_date=$(date +%Y%m%d_%H%M)
    local commit_hash=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    local branch_name=$(git symbolic-ref --short HEAD 2>/dev/null | sed 's/[^a-zA-Z0-9._-]/_/g' || echo "unknown")

    # Generate output filename
    local output_zip="$anykernel_dir/WMKernel-ksunext-susfs-dev_${current_date}_${commit_hash}_${branch_name}.zip"

    print_status "Creating flashable zip..."
    print_status "Source: $source_zip"
    print_status "Output: $output_zip"

    # Copy the source zip to the new location
    if cp "$source_zip" "$output_zip"; then
        print_success "Base zip copied successfully"
    else
        print_error "Failed to copy base zip"
        return 1
    fi

    # Check if zip command exists
    if ! command_exists "zip"; then
        print_error "zip command not found. Please install zip package"
        return 1
    fi

    # Add the kernel image to the zip
    print_status "Adding kernel image to zip..."
    if cd "$PREFIX" && zip -j "$output_zip" "$kernel_image" > /dev/null 2>&1; then
        print_success "Kernel image added to zip successfully"
        cd "$PREFIX"  # Return to original directory
    else
        print_error "Failed to add kernel image to zip"
        cd "$PREFIX"  # Return to original directory even on failure
        return 1
    fi

    # Display final zip information
    if [ -f "$output_zip" ]; then
        local zip_size=$(ls -lh "$output_zip" | awk '{print $5}')
        print_success "Flashable zip created successfully!"
        echo -e "  ${CYAN}Location:${NC} $output_zip"
        echo -e "  ${CYAN}Size:${NC} $zip_size"
        return 0
    else
        print_error "Flashable zip creation failed"
        return 1
    fi
}

# Start timing
BUILD_START_TIME=$(date +%s)

print_section "ANDROID KERNEL BUILD SCRIPT"
print_status "Starting build process for Android Kernel 4.14"

# Define paths and toolchain
PREFIX="$(pwd)"
print_status "Working directory: $PREFIX"

# Check if custom LLVM toolchain exists, otherwise use default
print_section "TOOLCHAIN DETECTION"
if [ -d "/home/zears/tc-build/build/llvm/final/bin" ]; then
    CLANG_DIR="/home/zears/tc-build/build/llvm/final"
    print_success "Found custom LLVM toolchain: $CLANG_DIR"

    # Check clang version
    if [ -x "$CLANG_DIR/bin/clang" ]; then
        CLANG_VERSION=$("$CLANG_DIR/bin/clang" --version | head -n1)
        print_status "Clang version: $CLANG_VERSION"
    fi
else
    CLANG_DIR="${PREFIX}/toolchain/clang/host/linux-x86/clang-r383902"
    print_warning "Custom toolchain not found, using default: $CLANG_DIR"

    if [ ! -d "$CLANG_DIR" ]; then
        print_error "Default toolchain directory not found!"
        exit 1
    fi
fi

# Verify essential tools exist
print_section "TOOLCHAIN VERIFICATION"
export PATH="$CLANG_DIR/bin:$PATH"
export ARCH=arm64

REQUIRED_TOOLS=("clang" "llvm-ar" "llvm-nm" "ld.lld" "llvm-objcopy" "llvm-objdump" "llvm-strip")
for tool in "${REQUIRED_TOOLS[@]}"; do
    if command_exists "$tool"; then
        print_success "$tool found"
    else
        print_error "$tool not found in PATH"
        exit 1
    fi
done

# Check for ccache and set CC accordingly
if command_exists "ccache"; then
    print_success "ccache found - build acceleration enabled"
    ccache -z > /dev/null 2>&1  # Reset stats
    CC_CMD="ccache clang"
    CCACHE_AVAILABLE=true
else
    print_warning "ccache not found - builds will be slower"
    CC_CMD="clang"
    CCACHE_AVAILABLE=false
fi

# Build configuration
print_section "BUILD CONFIGURATION"
export KCFLAGS=-w
export CONFIG_SECTION_MISMATCH_WARN_ONLY=y

print_status "Architecture: arm64"
print_status "Suppressing warnings: enabled"
print_status "Section mismatch warnings only: enabled"

# Configure kernel
print_section "KERNEL CONFIGURATION"
print_status "Configuring kernel with a22_defconfig..."

if make -C "$PREFIX" O="$PREFIX/out" ARCH=arm64 a22_defconfig; then
    print_success "Kernel configuration completed"
else
    print_error "Kernel configuration failed"
    exit 1
fi

# Build kernel
print_section "KERNEL COMPILATION"
print_status "Starting compilation with 16 parallel jobs..."
print_status "This may take several minutes depending on your hardware..."

# Store build command for reference
BUILD_CMD="make -j16 ARCH=arm64 SUBARCH=arm64 O=out \
CC=\"$CC_CMD\" \
AR=\"llvm-ar\" \
NM=\"llvm-nm\" \
LD=\"ld.lld\" \
OBJCOPY=\"llvm-objcopy\" \
OBJDUMP=\"llvm-objdump\" \
STRIP=\"llvm-strip\" \
CLANG_TRIPLE=\"aarch64-linux-gnu-\" \
CROSS_COMPILE=\"aarch64-linux-gnu-\" \
CROSS_COMPILE_ARM32=\"arm-linux-gnueabi-\" \
CROSS_COMPILE_COMPAT=\"arm-linux-gnueabi-\" \
LLVM=1 \
LLVM_IAS=1 \
INSTALL_MOD_STRIP=1 \
KCFLAGS=-w \
CONFIG_SECTION_MISMATCH_WARN_ONLY=y \
KBUILD_BUILD_USER=\"$(git rev-parse --short HEAD | cut -c1-7)\" \
KBUILD_BUILD_HOST=\"$(git symbolic-ref --short HEAD)\""

if eval $BUILD_CMD; then
    print_success "Kernel compilation completed successfully"
else
    print_error "Kernel compilation failed"
    exit 1
fi

# Copy the built kernel image
print_section "POST-BUILD OPERATIONS"
print_status "Copying kernel image..."

if [ -f "out/arch/arm64/boot/Image" ]; then
    cp out/arch/arm64/boot/Image "$PREFIX/arch/arm64/boot/Image"
    print_success "Kernel image copied to arch/arm64/boot/Image"
else
    print_error "Kernel image not found at expected location"
    exit 1
fi

# Show ccache statistics
if [ "$CCACHE_AVAILABLE" = true ]; then
    print_section "CCACHE STATISTICS"
    ccache -s
fi

# Build completion
BUILD_END_TIME=$(date +%s)
show_build_info $BUILD_START_TIME $BUILD_END_TIME

# Create flashable zip
create_flashable_zip

print_section "BUILD COMPLETED"
print_success "Android kernel build finished successfully!"
print_status "Kernel image ready at: arch/arm64/boot/Image"