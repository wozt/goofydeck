#!/usr/bin/env bash
# Description: Enhanced MDI download with multiple methods and individual file support
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEST="${ROOT}/assets/mdi"
REPO_URL="https://github.com/Templarian/MaterialDesign.git"
GITHUB_RAW_URL="https://raw.githubusercontent.com/Templarian/MaterialDesign/master/svg"
CDN_URL="https://cdn.jsdelivr.net/npm/@mdi/svg@latest/svg"

# Default values
MODE="all"
ICON_NAME=""
METHOD="auto"
VERBOSE=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

usage() {
    cat << EOF
${BLUE}Enhanced MDI Icon Downloader${NC}
${BLUE}=============================${NC}

${YELLOW}USAGE:${NC}
    $0 [OPTIONS] [MODE] [ICON_NAME]

${YELLOW}MODES:${NC}
    ${GREEN}all${NC}         Download all MDI icons (original behavior)
    ${GREEN}individual${NC}  Download specific icon(s)
    ${GREEN}test${NC}        Test all download methods

${YELLOW}OPTIONS:${NC}
    ${GREEN}-h, --help${NC}         Show this help message
    ${GREEN}-m, --method METHOD${NC}  Download method (auto|github|git|cdn)
    ${GREEN}-v, --verbose${NC}       Enable verbose output
    ${GREEN}-d, --dest DIR${NC}      Destination directory (default: ${DEST})

${YELLOW}METHODS:${NC}
    ${GREEN}auto${NC}      Try methods in order: cdn -> github -> git (default)
    ${GREEN}github${NC}    Use GitHub Raw URL only (fastest, lightweight)
    ${GREEN}git${NC}       Use git sparse checkout (requires git, heavier)
    ${GREEN}cdn${NC}       Use jsdelivr CDN only (may have network issues)

${YELLOW}EXAMPLES:${NC}
    ${BLUE}$0${NC}                                    # Download all icons
    ${BLUE}$0 all${NC}                                # Download all icons (explicit)
    ${BLUE}$0 individual apps${NC}                      # Download apps.svg only
    ${BLUE}$0 individual apps bed bullseye${NC}         # Download multiple icons
    ${BLUE}$0 -m github individual apps${NC}            # Force GitHub method
    ${BLUE}$0 -v individual apps${NC}                  # Verbose individual download
    ${BLUE}$0 test${NC}                               # Test all methods
    ${BLUE}$0 -d /tmp/icons individual apps${NC}        # Custom destination

${YELLOW}NOTES:${NC}
    • ${GREEN}github${NC} method is recommended for Raspberry Pi Zero 2W and low-memory devices
    • ${GREEN}cdn${NC} method is fastest but may fail on some networks
    • ${GREEN}git${NC} method downloads all icons even for individual mode
    • Existing files are always skipped to save bandwidth

EOF
    exit 0
}

log_info() {
    if [ "${VERBOSE}" = true ]; then
        echo -e "${BLUE}[INFO]${NC} $1"
    fi
}

log_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

log_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

log_error() {
    echo -e "${RED}❌ $1${NC}"
}

# Method 1: GitHub Raw URL (most reliable for individual downloads)
download_github_raw() {
    local icon="$1"
    local target="${DEST}/${icon}.svg"
    
    log_info "Attempting GitHub Raw method for ${icon}.svg"
    
    if [ -f "${target}" ]; then
        log_success "${icon}.svg already exists"
        return 0
    fi
    
    mkdir -p "${DEST}"
    
    # Ensure we have the required tools
    if command -v curl >/dev/null 2>&1; then
        if curl -s --connect-timeout 10 --max-time 30 "${GITHUB_RAW_URL}/${icon}.svg" -o "${target}" 2>/dev/null; then
            if [ -f "${target}" ] && [ -s "${target}" ]; then
                log_success "Downloaded ${icon}.svg via GitHub Raw"
                return 0
            else
                rm -f "${target}"  # Remove empty file
                log_warning "GitHub Raw: downloaded file is empty or invalid"
                return 1
            fi
        else
            log_warning "GitHub Raw: curl download failed"
            return 1
        fi
    elif command -v wget >/dev/null 2>&1; then
        if wget -q --timeout=30 --tries=3 "${GITHUB_RAW_URL}/${icon}.svg" -O "${target}" 2>/dev/null; then
            if [ -f "${target}" ] && [ -s "${target}" ]; then
                log_success "Downloaded ${icon}.svg via GitHub Raw"
                return 0
            else
                rm -f "${target}"  # Remove empty file
                log_warning "GitHub Raw: downloaded file is empty or invalid"
                return 1
            fi
        else
            log_warning "GitHub Raw: wget download failed"
            return 1
        fi
    else
        log_error "Neither curl nor wget available for GitHub Raw method"
        return 1
    fi
}

# Method 2: jsdelivr CDN
download_cdn() {
    local icon="$1"
    local target="${DEST}/${icon}.svg"
    
    log_info "Attempting CDN method for ${icon}.svg"
    
    if [ -f "${target}" ]; then
        log_success "${icon}.svg already exists"
        return 0
    fi
    
    mkdir -p "${DEST}"
    
    if command -v curl >/dev/null 2>&1; then
        if curl -s --connect-timeout 10 --max-time 30 "${CDN_URL}/${icon}.svg" -o "${target}" 2>/dev/null; then
            log_success "Downloaded ${icon}.svg via CDN"
            return 0
        else
            log_warning "CDN: curl download failed"
            return 1
        fi
    elif command -v wget >/dev/null 2>&1; then
        if wget -q --timeout=30 --tries=3 "${CDN_URL}/${icon}.svg" -O "${target}" 2>/dev/null; then
            log_success "Downloaded ${icon}.svg via CDN"
            return 0
        else
            log_warning "CDN: wget download failed"
            return 1
        fi
    else
        log_error "Neither curl nor wget available"
        return 1
    fi
}

# Method 3: Git sparse checkout (heavier but complete)
download_git() {
    local icon="$1"
    local target="${DEST}/${icon}.svg"
    
    log_info "Attempting Git method for ${icon}.svg"
    
    if [ -f "${target}" ]; then
        log_success "${icon}.svg already exists"
        return 0
    fi
    
    # Check if git is available
    command -v git >/dev/null 2>&1 || { 
        log_error "git is required for Git method"; 
        return 1; 
    }
    
    # Create temporary directory safely using a fixed pattern
    local TMP_DIR="/tmp/goofydeck_mdi_$$"
    if ! mkdir -p "${TMP_DIR}"; then
        log_error "Failed to create temporary directory"
        return 1
    fi
    
    # Simple cleanup at the end
    local cleanup_done=0
    cleanup_temp() {
        if [ "${cleanup_done}" = "0" ] && [ -d "${TMP_DIR}" ]; then
            rm -rf "${TMP_DIR}"
            cleanup_done=1
        fi
    }
    
    log_info "Cloning repository (sparse)..."
    if git -c advice.detachedHead=false clone --depth 1 --filter=blob:none --sparse "${REPO_URL}" "${TMP_DIR}/repo" 2>/dev/null; then
        log_info "Setting up sparse checkout..."
        if git -C "${TMP_DIR}/repo" sparse-checkout set svg 2>/dev/null; then
            log_info "Checking out files..."
            if git -C "${TMP_DIR}/repo" checkout 2>/dev/null; then
                local source_file="${TMP_DIR}/repo/svg/${icon}.svg"
                if [ -f "${source_file}" ]; then
                    cp "${source_file}" "${target}"
                    log_success "Downloaded ${icon}.svg via Git"
                    cleanup_temp
                    return 0
                else
                    log_warning "Git: Source file not found"
                    cleanup_temp
                    return 1
                fi
            else
                log_warning "Git: Checkout failed"
                cleanup_temp
                return 1
            fi
        else
            log_warning "Git: Sparse checkout setup failed"
            cleanup_temp
            return 1
        fi
    else
        log_warning "Git: Clone failed"
        cleanup_temp
        return 1
    fi
}

# Auto method: try all methods in order
download_auto() {
    local icon="$1"
    
    log_info "Auto mode: trying methods for ${icon}.svg"
    
    # Try CDN first (fastest)
    if download_cdn "${icon}"; then
        return 0
    fi
    
    # Try GitHub Raw (most reliable)
    if download_github_raw "${icon}"; then
        return 0
    fi
    
    # Try Git as last resort
    if download_git "${icon}"; then
        return 0
    fi
    
    log_error "All methods failed for ${icon}.svg"
    return 1
}

# Download individual icon(s)
download_individual() {
    local icons=("$@")
    local success_count=0
    local fail_count=0
    
    log_info "Downloading ${#icons[@]} icon(s) using ${METHOD} method..."
    
    for icon in "${icons[@]}"; do
        case "${METHOD}" in
            "github")
                if download_github_raw "${icon}"; then
                    success_count=$((success_count + 1))
                else
                    fail_count=$((fail_count + 1))
                fi
                ;;
            "cdn")
                if download_cdn "${icon}"; then
                    success_count=$((success_count + 1))
                else
                    fail_count=$((fail_count + 1))
                fi
                ;;
            "git")
                if download_git "${icon}"; then
                    success_count=$((success_count + 1))
                else
                    fail_count=$((fail_count + 1))
                fi
                ;;
            "auto"|*)
                if download_auto "${icon}"; then
                    success_count=$((success_count + 1))
                else
                    fail_count=$((fail_count + 1))
                fi
                ;;
        esac
    done
    
    echo
    log_info "Download completed: ${success_count} success, ${fail_count} failed"
    return ${fail_count}
}

# Download all icons (original behavior)
download_all() {
    command -v git >/dev/null 2>&1 || { log_error "git is required for full download"; exit 1; }
    
    mkdir -p "${DEST}"
    
    # Create temporary directory safely using a fixed pattern
    local TMP_DIR="/tmp/goofydeck_mdi_all_$$"
    if ! mkdir -p "${TMP_DIR}"; then
        log_error "Failed to create temporary directory"
        exit 1
    fi
    
    # Simple cleanup at the end
    cleanup_all_temp() {
        if [ -d "${TMP_DIR}" ]; then
            rm -rf "${TMP_DIR}"
        fi
    }
    
    log_info "Cloning MaterialDesign repository (sparse, svg only)..."
    if git -c advice.detachedHead=false clone --depth 1 --filter=blob:none --sparse "${REPO_URL}" "${TMP_DIR}/repo" >&2; then
        log_info "Setting up sparse checkout for svg directory..."
        if git -C "${TMP_DIR}/repo" sparse-checkout set svg >&2; then
            log_info "Checking out files..."
            if git -C "${TMP_DIR}/repo" checkout >&2; then
                local SVG_DIR="${TMP_DIR}/repo/svg"
                if [ -d "${SVG_DIR}" ]; then
                    local downloaded=0
                    local skipped=0
                    
                    log_info "Copying SVG files to ${DEST}..."
                    for file in "${SVG_DIR}"/*.svg; do
                        [ -e "${file}" ] || continue
                        local name="$(basename "${file}")"
                        local target="${DEST}/${name}"
                        
                        if [ -f "${target}" ]; then
                            skipped=$((skipped + 1))
                        else
                            cp "${file}" "${target}"
                            downloaded=$((downloaded + 1))
                        fi
                    done
                    
                    log_success "Download completed: ${downloaded} new files, ${skipped} already present"
                    cleanup_all_temp
                    return 0
                else
                    log_error "SVG directory not found in clone"
                    cleanup_all_temp
                    exit 1
                fi
            else
                log_error "Git checkout failed"
                cleanup_all_temp
                exit 1
            fi
        else
            log_error "Git sparse checkout setup failed"
            cleanup_all_temp
            exit 1
        fi
    else
        log_error "Git clone failed"
        cleanup_all_temp
        exit 1
    fi
}

# Test all methods
test_methods() {
    local test_icon="apps"
    
    echo -e "${BLUE}Testing download methods with ${test_icon}.svg...${NC}"
    echo -e "${BLUE}=============================================${NC}"
    
    # Remove test file if exists
    rm -f "${DEST}/${test_icon}.svg"
    
    # Test CDN
    echo -e "\n${YELLOW}Testing CDN method...${NC}"
    if download_cdn "${test_icon}"; then
        log_success "CDN method: PASSED"
    else
        log_error "CDN method: FAILED"
    fi
    
    # Remove test file
    rm -f "${DEST}/${test_icon}.svg"
    
    # Test GitHub Raw
    echo -e "\n${YELLOW}Testing GitHub Raw method...${NC}"
    if download_github_raw "${test_icon}"; then
        log_success "GitHub Raw method: PASSED"
    else
        log_error "GitHub Raw method: FAILED"
    fi
    
    # Remove test file
    rm -f "${DEST}/${test_icon}.svg"
    
    # Test Git
    echo -e "\n${YELLOW}Testing Git method...${NC}"
    if download_git "${test_icon}"; then
        log_success "Git method: PASSED"
    else
        log_error "Git method: FAILED"
    fi
    
    # Clean up
    rm -f "${DEST}/${test_icon}.svg"
    
    echo -e "\n${BLUE}Test completed!${NC}"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -m|--method)
            METHOD="$2"
            shift 2
            ;;
        -d|--dest)
            DEST="$2"
            shift 2
            ;;
        all)
            MODE="all"
            shift
            ;;
        individual)
            MODE="individual"
            shift
            ;;
        test)
            MODE="test"
            shift
            ;;
        -*)
            log_error "Unknown option: $1"
            usage
            ;;
        *)
            # This is an icon name
            break
            ;;
    esac
done

# Validate method
case "${METHOD}" in
    auto|github|git|cdn)
        # Valid method
        ;;
    *)
        log_error "Invalid method: ${METHOD}"
        usage
        ;;
esac

# Main execution
case "${MODE}" in
    "all")
        log_info "Starting full download using ${METHOD} method..."
        download_all
        ;;
    "individual")
        if [ $# -eq 0 ]; then
            log_error "No icon names specified for individual mode"
            usage
        fi
        download_individual "$@"
        ;;
    "test")
        test_methods
        ;;
    *)
        log_error "Unknown mode: ${MODE}"
        usage
        ;;
esac
