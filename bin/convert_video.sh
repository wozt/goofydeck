#!/bin/bash

# Frame-by-frame video conversion script with dithering and 256-color quantization
# Usage: ./convert_video.sh [--size=720|480|360] [--fps=1-60] [-d|--dither] <filename.mp4>
# 
# Features:
# - Convert to target format (720p=1280x720, 480p=854x480, 360p=640x360) OR keep original resolution
# - Smart 16:9 center crop (if resizing)
# - Frame-by-frame processing with Floyd-Steinberg dithering (optional)
# - 256-color quantization PER frame (systematic)
# - Recomposition with original audio
# - Optimized compression

# Display functions
print_info() { echo -e "\033[1;34m[INFO]\033[0m $1"; }
print_success() { echo -e "\033[1;32m[SUCCESS]\033[0m $1"; }
print_error() { echo -e "\033[1;31m[ERROR]\033[0m $1"; }
print_warning() { echo -e "\033[1;33m[WARNING]\033[0m $1"; }

# Check dependencies
check_dependencies() {
    local deps=("ffmpeg" "ffprobe" "convert" "identify")
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
            print_error "Missing dependency: $dep"
            print_info "Install with: sudo apt install ffmpeg imagemagick"
            exit 1
        fi
    done
}

# Parser les arguments
parse_args() {
    if [[ $# -lt 1 || $# -gt 4 ]]; then
        print_error "Usage: $0 [--size=720|480|360] [--fps=1-60] [-d|--dither] <filename.mp4>"
        print_info "Examples:"
        print_info "  $0 video.mp4"
        print_info "  $0 --size=720 video.mp4"
        print_info "  $0 --size=480 --fps=30 video.mkv"
        print_info "  $0 --size=360 --fps=15 video.avi"
        print_info "  $0 -d video.mp4  # with Floyd-Steinberg dithering"
        print_info "  $0 --size=720 -d --fps=30 video.mp4  # conversion + dithering"
        exit 1
    fi
    
    TARGET_SIZE=""
    TARGET_FPS=""
    INPUT_FILE=""
    DITHER_MODE=""  # "" = no dithering, "FloydSteinberg" = dithering enabled
    
    # Parse arguments
    for arg in "$@"; do
        if [[ "$arg" =~ ^--size=(720|480|360)$ ]]; then
            TARGET_SIZE="${BASH_REMATCH[1]}"
        elif [[ "$arg" =~ ^--fps=([1-9]|[1-5][0-9]|60)$ ]]; then
            TARGET_FPS="${BASH_REMATCH[1]}"
        elif [[ "$arg" == "-d" || "$arg" == "--dither" ]]; then
            DITHER_MODE="FloydSteinberg"
        elif [[ ! "$arg" =~ ^-- ]]; then
            INPUT_FILE="$arg"
        fi
    done
    
    # Checks
    if [[ -z "$INPUT_FILE" ]]; then
        print_error "Input file required"
        exit 1
    fi
    
    if [[ ! -f "$INPUT_FILE" ]]; then
        print_error "File not found: $INPUT_FILE"
        exit 1
    fi
    
    INPUT_DIR="$(dirname "$INPUT_FILE")"
    INPUT_BASE="$(basename "$INPUT_FILE")"
    INPUT_NAME="${INPUT_BASE%.*}"
    
    # Build output name with FPS if specified
    if [[ -n "$TARGET_FPS" ]]; then
        if [[ -n "$TARGET_SIZE" ]]; then
            OUTPUT_FILE="${INPUT_DIR}/${INPUT_NAME}_converted_${TARGET_SIZE}p_${TARGET_FPS}fps.mp4"
        else
            OUTPUT_FILE="${INPUT_DIR}/${INPUT_NAME}_converted_${TARGET_FPS}fps.mp4"
        fi
    else
        if [[ -n "$TARGET_SIZE" ]]; then
            OUTPUT_FILE="${INPUT_DIR}/${INPUT_NAME}_converted_${TARGET_SIZE}p.mp4"
        else
            OUTPUT_FILE="${INPUT_DIR}/${INPUT_NAME}_converted.mp4"
        fi
    fi
    
    print_info "Input file: $INPUT_FILE"
    print_info "Output file: $OUTPUT_FILE"
    if [[ -n "$TARGET_SIZE" ]]; then
        print_info "Target size: ${TARGET_SIZE}p"
    else
        print_info "Size: original resolution"
    fi
    if [[ -n "$TARGET_FPS" ]]; then
        print_info "Target FPS: $TARGET_FPS"
    fi
    if [[ -n "$DITHER_MODE" ]]; then
        print_info "Dithering: enabled (Floyd-Steinberg)"
    else
        print_info "Dithering: disabled"
    fi
}

# Get video information
get_video_info() {
    local info
    info=$(ffprobe -v quiet -print_format json -show_streams "$INPUT_FILE")
    
    WIDTH=$(echo "$info" | jq -r '.streams[] | select(.codec_type=="video") | .width' 2>/dev/null || echo "0")
    HEIGHT=$(echo "$info" | jq -r '.streams[] | select(.codec_type=="video") | .height' 2>/dev/null || echo "0")
    FPS=$(echo "$info" | jq -r '.streams[] | select(.codec_type=="video") | .r_frame_rate' 2>/dev/null || echo "25/1")
    DURATION=$(echo "$info" | jq -r '.streams[] | select(.codec_type=="video") | .duration' 2>/dev/null || echo "0")
    
    if [[ "$WIDTH" == "0" || "$HEIGHT" == "0" ]]; then
        WIDTH=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=width -of csv=p=0 "$INPUT_FILE")
        HEIGHT=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=height -of csv=p=0 "$INPUT_FILE")
        FPS=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$INPUT_FILE")
        DURATION=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$INPUT_FILE")
    fi
    
    print_info "Original dimensions: ${WIDTH}x${HEIGHT}"
    print_info "FPS: $FPS"
    print_info "Duration: ${DURATION}s"
}

# Set target dimensions
set_target_dimensions() {
    if [[ -n "$TARGET_SIZE" ]]; then
        case "$TARGET_SIZE" in
            720)
                TARGET_WIDTH=1280
                TARGET_HEIGHT=720
                ;;
            480)
                TARGET_WIDTH=854
                TARGET_HEIGHT=480
                ;;
            360)
                TARGET_WIDTH=640
                TARGET_HEIGHT=360
                ;;
        esac
        
        print_info "Target format: ${TARGET_WIDTH}x${TARGET_HEIGHT} (${TARGET_SIZE}p)"
    else
        # Keep original resolution
        TARGET_WIDTH="$WIDTH"
        TARGET_HEIGHT="$HEIGHT"
        print_info "Target format: ${TARGET_WIDTH}x${TARGET_HEIGHT} (original resolution)"
    fi
}

# No crop - simple resizing
calculate_crop_dimensions() {
    # Use target dimensions directly without crop
    CROP_WIDTH="$TARGET_WIDTH"
    CROP_HEIGHT="$TARGET_HEIGHT"
    CROP_X=0
    CROP_Y=0
    print_info "Direct resize: ${CROP_WIDTH}x${CROP_HEIGHT} (no crop)"
}

# Main function
main() {
    print_info "Starting frame-by-frame video conversion..."
    
    parse_args "$@"
    check_dependencies
    get_video_info
    set_target_dimensions
    calculate_crop_dimensions
    
    # Create temporary directory
    TEMP_DIR=$(mktemp -d -p /dev/shm/ convert_video_XXXXXX)
    if [[ ! -d "$TEMP_DIR" ]]; then
        print_error "Unable to create temporary directory"
        exit 1
    fi
    
    print_info "Temporary directory: $TEMP_DIR"
    
    # Cleanup on interruption
    cleanup() {
        print_info "Cleaning up..."
        rm -rf "$TEMP_DIR"
    }
    trap cleanup EXIT INT TERM
    
    local cropped_video="${TEMP_DIR}/cropped_video.mp4"
    
    # STEP 1: Convert and resize video
    if [[ -n "$TARGET_SIZE" ]]; then
        print_info "Step 1: Converting to ${TARGET_SIZE}p format..."
    else
        print_info "Step 1: Resizing video..."
    fi
    
    local ffmpeg_cmd="ffmpeg -y -i \"$INPUT_FILE\" -vf \"scale=${TARGET_WIDTH}:${TARGET_HEIGHT}\" -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -movflags +faststart -threads 4"
    
    # Add FPS if specified
    if [[ -n "$TARGET_FPS" ]]; then
        ffmpeg_cmd="$ffmpeg_cmd -r $TARGET_FPS"
        print_info "Target FPS: $TARGET_FPS fps"
    fi
    
    # Limit to 10 seconds for testing - REMOVED FOR FINAL VERSION
    # ffmpeg_cmd="$ffmpeg_cmd -t 10"
    
    ffmpeg_cmd="$ffmpeg_cmd \"$cropped_video\""
    
    print_info "Command: $ffmpeg_cmd"
    
    if eval "$ffmpeg_cmd"; then
        if [[ -f "$cropped_video" ]]; then
            print_success "Video converted and cropped: $cropped_video"
            ls -lh "$cropped_video"
        else
            print_error "File not created: $cropped_video"
            exit 1
        fi
    else
        print_error "Conversion failed"
        exit 1
    fi
    
    # STEP 2: Process individual frames
    print_info "Step 2: Frame-by-frame processing with 256 colors..."
    if [[ -n "$DITHER_MODE" ]]; then
        print_info "Floyd-Steinberg dithering enabled"
    else
        print_info "Dithering disabled"
    fi
    
    local frames_dir="${TEMP_DIR}/frames"
    local processed_dir="${TEMP_DIR}/processed"
    mkdir -p "$frames_dir" "$processed_dir"
    
    # Extract all frames (no more limitation for final version)
    ffmpeg -y -i "$cropped_video" "${frames_dir}/frame_%06d.png" 2>/dev/null
    
    # Process each frame
    local frame_count=0
    for frame_file in "${frames_dir}"/*.png; do
        if [[ -f "$frame_file" ]]; then
            local frame_name=$(basename "$frame_file")
            local output_frame="${processed_dir}/${frame_name}"
            
            # Apply 256 colors systematically, optional dithering
            local convert_cmd="convert \"$frame_file\""
            
            if [[ -n "$DITHER_MODE" ]]; then
                convert_cmd="$convert_cmd -dither FloydSteinberg"
            fi
            
            convert_cmd="$convert_cmd -colors 256 -depth 8 \"$output_frame\""
            
            eval "$convert_cmd" 2>/dev/null
            
            ((frame_count++))
            
            # Show progress
            if [[ $((frame_count % 10)) -eq 0 ]]; then
                echo -ne "\rFrames processed: $frame_count"
            fi
        fi
    done
    
    echo # New line
    print_success "Processing completed: $frame_count frames processed"
    
    # STEP 3: Recompose video
    print_info "Step 3: Recomposing video..."
    
    local temp_video="${TEMP_DIR}/temp_video.mp4"
    local final_fps="$FPS"
    if [[ -n "$TARGET_FPS" ]]; then
        final_fps="$TARGET_FPS"
    fi
    
    # Create video from processed frames
    ffmpeg -y \
        -framerate "$final_fps" \
        -i "${processed_dir}/frame_%06d.png" \
        -c:v libx264 \
        -preset medium \
        -crf 23 \
        -pix_fmt yuv420p \
        -movflags +faststart \
        -threads 4 \
        "$temp_video" 2>/dev/null
    
    if [[ -f "$temp_video" ]]; then
        # Add original audio
        ffmpeg -y \
            -i "$temp_video" \
            -i "$INPUT_FILE" \
            -c:v copy \
            -c:a aac \
            -map 0:v:0 \
            -map 1:a:0 \
            -shortest \
            "$OUTPUT_FILE" 2>/dev/null
        
        if [[ -f "$OUTPUT_FILE" ]]; then
            print_success "Final video created: $OUTPUT_FILE"
            
            # Show statistics
            if command -v stat &> /dev/null; then
                local input_size=$(stat -f%z "$INPUT_FILE" 2>/dev/null || stat -c%s "$INPUT_FILE" 2>/dev/null)
                local output_size=$(stat -f%z "$OUTPUT_FILE" 2>/dev/null || stat -c%s "$OUTPUT_FILE" 2>/dev/null)
                
                if [[ -n "$input_size" && -n "$output_size" ]]; then
                    local ratio=$(echo "scale=1; $output_size * 100 / $input_size" | bc 2>/dev/null || echo "N/A")
                    print_info "Input size: $(numfmt --to=iec $input_size)"
                    print_info "Output size: $(numfmt --to=iec $output_size)"
                    print_info "Ratio: ${ratio}%"
                fi
            fi
        else
            print_error "Failed to add audio"
            exit 1
        fi
    else
        print_error "Video recomposition failed"
        exit 1
    fi
}

# Run script
main "$@"
