#!/bin/bash

# Script de conversion vidéo frame par frame avec dithering et quantification 256 couleurs
# Usage: ./convert_video.sh [--size=720|480|360] [--fps=1-60] [-d|--dither] <filename.mp4>
# 
# Fonctionnalités:
# - Conversion au format cible (720p=1280x720, 480p=854x480, 360p=640x360) OU conservation résolution originale
# - Crop intelligent 16:9 au centre (si redimensionnement)
# - Traitement frame par frame avec dithering Floyd-Steinberg (optionnel)
# - Quantification 256 couleurs PAR frame (systématique)
# - Recomposition avec audio original
# - Compression optimisée

# Fonctions d'affichage
print_info() { echo -e "\033[1;34m[INFO]\033[0m $1"; }
print_success() { echo -e "\033[1;32m[SUCCESS]\033[0m $1"; }
print_error() { echo -e "\033[1;31m[ERROR]\033[0m $1"; }
print_warning() { echo -e "\033[1;33m[WARNING]\033[0m $1"; }

# Vérifier les dépendances
check_dependencies() {
    local deps=("ffmpeg" "ffprobe" "convert" "identify")
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
            print_error "Dépendance manquante: $dep"
            print_info "Installez avec: sudo apt install ffmpeg imagemagick"
            exit 1
        fi
    done
}

# Parser les arguments
parse_args() {
    if [[ $# -lt 1 || $# -gt 4 ]]; then
        print_error "Usage: $0 [--size=720|480|360] [--fps=1-60] [-d|--dither] <filename.mp4>"
        print_info "Exemples:"
        print_info "  $0 video.mp4"
        print_info "  $0 --size=720 video.mp4"
        print_info "  $0 --size=480 --fps=30 video.mkv"
        print_info "  $0 --size=360 --fps=15 video.avi"
        print_info "  $0 -d video.mp4  # avec dithering Floyd-Steinberg"
        print_info "  $0 --size=720 -d --fps=30 video.mp4  # conversion + dithering"
        exit 1
    fi
    
    TARGET_SIZE=""
    TARGET_FPS=""
    INPUT_FILE=""
    DITHER_MODE=""  # "" = pas de dithering, "FloydSteinberg" = dithering activé
    
    # Parser les arguments
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
    
    # Vérifications
    if [[ -z "$INPUT_FILE" ]]; then
        print_error "Fichier d'entrée requis"
        exit 1
    fi
    
    if [[ ! -f "$INPUT_FILE" ]]; then
        print_error "Fichier introuvable: $INPUT_FILE"
        exit 1
    fi
    
    INPUT_DIR="$(dirname "$INPUT_FILE")"
    INPUT_BASE="$(basename "$INPUT_FILE")"
    INPUT_NAME="${INPUT_BASE%.*}"
    
    # Construire le nom de sortie avec FPS si spécifié
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
    
    print_info "Fichier d'entrée: $INPUT_FILE"
    print_info "Fichier de sortie: $OUTPUT_FILE"
    if [[ -n "$TARGET_SIZE" ]]; then
        print_info "Taille cible: ${TARGET_SIZE}p"
    else
        print_info "Taille: résolution originale"
    fi
    if [[ -n "$TARGET_FPS" ]]; then
        print_info "FPS cible: $TARGET_FPS"
    fi
    if [[ -n "$DITHER_MODE" ]]; then
        print_info "Dithering: activé (Floyd-Steinberg)"
    else
        print_info "Dithering: désactivé"
    fi
}

# Obtenir les informations vidéo
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
    
    print_info "Dimensions originales: ${WIDTH}x${HEIGHT}"
    print_info "FPS: $FPS"
    print_info "Durée: ${DURATION}s"
}

# Définir les dimensions cibles
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
        
        print_info "Format cible: ${TARGET_WIDTH}x${TARGET_HEIGHT} (${TARGET_SIZE}p)"
    else
        # Garder la résolution originale
        TARGET_WIDTH="$WIDTH"
        TARGET_HEIGHT="$HEIGHT"
        print_info "Format cible: ${TARGET_WIDTH}x${TARGET_HEIGHT} (résolution originale)"
    fi
}

# Pas de crop - simple redimensionnement
calculate_crop_dimensions() {
    # Utiliser directement les dimensions cibles sans crop
    CROP_WIDTH="$TARGET_WIDTH"
    CROP_HEIGHT="$TARGET_HEIGHT"
    CROP_X=0
    CROP_Y=0
    print_info "Redimensionnement direct: ${CROP_WIDTH}x${CROP_HEIGHT} (pas de crop)"
}

# Fonction principale
main() {
    print_info "Démarrage de la conversion vidéo frame par frame..."
    
    parse_args "$@"
    check_dependencies
    get_video_info
    set_target_dimensions
    calculate_crop_dimensions
    
    # Créer le répertoire temporaire
    TEMP_DIR=$(mktemp -d -p /dev/shm/ convert_video_XXXXXX)
    if [[ ! -d "$TEMP_DIR" ]]; then
        print_error "Impossible de créer le répertoire temporaire"
        exit 1
    fi
    
    print_info "Répertoire temporaire: $TEMP_DIR"
    
    # Nettoyage en cas d'interruption
    cleanup() {
        print_info "Nettoyage..."
        rm -rf "$TEMP_DIR"
    }
    trap cleanup EXIT INT TERM
    
    local cropped_video="${TEMP_DIR}/cropped_video.mp4"
    
    # ÉTAPE 1: Convertir et redimensionner la vidéo
    if [[ -n "$TARGET_SIZE" ]]; then
        print_info "Étape 1: Conversion au format ${TARGET_SIZE}p..."
    else
        print_info "Étape 1: Conversion avec résolution originale..."
    fi
    
    local ffmpeg_cmd="ffmpeg -y -i \"$INPUT_FILE\" -vf \"scale=${TARGET_WIDTH}:${TARGET_HEIGHT}\" -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -movflags +faststart -threads 4"
    
    # Ajouter le FPS si spécifié
    if [[ -n "$TARGET_FPS" ]]; then
        ffmpeg_cmd="$ffmpeg_cmd -r $TARGET_FPS"
        print_info "FPS cible: $TARGET_FPS fps"
    fi
    
    # Limiter à 10 secondes pour le test - SUPPRIMÉ POUR LA VERSION FINALE
    # ffmpeg_cmd="$ffmpeg_cmd -t 10"
    
    ffmpeg_cmd="$ffmpeg_cmd \"$cropped_video\""
    
    print_info "Commande: $ffmpeg_cmd"
    
    if eval "$ffmpeg_cmd"; then
        if [[ -f "$cropped_video" ]]; then
            print_success "Vidéo convertie et croppée: $cropped_video"
            ls -lh "$cropped_video"
        else
            print_error "Fichier non créé: $cropped_video"
            exit 1
        fi
    else
        print_error "Échec de la conversion"
        exit 1
    fi
    
    # ÉTAPE 2: Traiter les frames individuellement
    print_info "Étape 2: Traitement frame par frame avec 256 couleurs..."
    if [[ -n "$DITHER_MODE" ]]; then
        print_info "Dithering Floyd-Steinberg activé"
    else
        print_info "Dithering désactivé"
    fi
    
    local frames_dir="${TEMP_DIR}/frames"
    local processed_dir="${TEMP_DIR}/processed"
    mkdir -p "$frames_dir" "$processed_dir"
    
    # Extraire toutes les frames (plus de limitation pour la version finale)
    ffmpeg -y -i "$cropped_video" "${frames_dir}/frame_%06d.png" 2>/dev/null
    
    # Traiter chaque frame
    local frame_count=0
    for frame_file in "${frames_dir}"/*.png; do
        if [[ -f "$frame_file" ]]; then
            local frame_name=$(basename "$frame_file")
            local output_frame="${processed_dir}/${frame_name}"
            
            # Appliquer 256 couleurs systématiquement, dithering optionnel
            local convert_cmd="convert \"$frame_file\""
            
            if [[ -n "$DITHER_MODE" ]]; then
                convert_cmd="$convert_cmd -dither FloydSteinberg"
            fi
            
            convert_cmd="$convert_cmd -colors 256 -depth 8 \"$output_frame\""
            
            eval "$convert_cmd" 2>/dev/null
            
            ((frame_count++))
            
            # Afficher la progression
            if [[ $((frame_count % 10)) -eq 0 ]]; then
                echo -ne "\rFrames traitées: $frame_count"
            fi
        fi
    done
    
    echo # Nouvelle ligne
    print_success "Traitement terminé: $frame_count frames traitées"
    
    # ÉTAPE 3: Recomposer la vidéo
    print_info "Étape 3: Recomposition de la vidéo..."
    
    local temp_video="${TEMP_DIR}/temp_video.mp4"
    local final_fps="$FPS"
    if [[ -n "$TARGET_FPS" ]]; then
        final_fps="$TARGET_FPS"
    fi
    
    # Créer la vidéo à partir des frames traitées
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
        # Ajouter l'audio original
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
            print_success "Vidéo finale créée: $OUTPUT_FILE"
            
            # Afficher les statistiques
            if command -v stat &> /dev/null; then
                local input_size=$(stat -f%z "$INPUT_FILE" 2>/dev/null || stat -c%s "$INPUT_FILE" 2>/dev/null)
                local output_size=$(stat -f%z "$OUTPUT_FILE" 2>/dev/null || stat -c%s "$OUTPUT_FILE" 2>/dev/null)
                
                if [[ -n "$input_size" && -n "$output_size" ]]; then
                    local ratio=$(echo "scale=1; $output_size * 100 / $input_size" | bc 2>/dev/null || echo "N/A")
                    print_info "Taille d'entrée: $(numfmt --to=iec $input_size)"
                    print_info "Taille de sortie: $(numfmt --to=iec $output_size)"
                    print_info "Ratio: ${ratio}%"
                fi
            fi
        else
            print_error "Échec de l'ajout de l'audio"
            exit 1
        fi
    else
        print_error "Échec de la recomposition vidéo"
        exit 1
    fi
}

# Lancer le script
main "$@"
