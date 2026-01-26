#!/usr/bin/env bash
set -eo pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOCK_PATH="/tmp/ulanzi_device.sock"
CONTROL_SOCKET="/tmp/play_rendered_video_control.sock"
MOUNT_POINT="/dev/shm/tmpvideoplayback"

# Variables par défaut
DELAY_MS=33
FOLDER=""
MOUNTED=false
CACHE_MODE=false

# Variables d'état du démon (fichiers temporaires pour partage entre processus)
STATE_DIR="/dev/shm/play_rendered_video_state_$$"
CURRENT_FRAME_FILE="$STATE_DIR/current_frame"
PLAYING_FILE="$STATE_DIR/playing"
PAUSED_FILE="$STATE_DIR/paused"
MAIN_PID_FILE="$STATE_DIR/main_pid"

# Fonction d'affichage de l'aide
show_help() {
    echo "Usage: $0 [options] <folder>"
    echo ""
    echo "Joue une vidéo rendue depuis un répertoire directement vers le démon."
    echo ""
    echo "Options:"
    echo "  -d, --delay=MS     Délai entre chaque frame en millisecondes (défaut: 33)"
    echo "  -c, --cache        Copier le dossier dans /dev/shm pour de meilleures performances"
    echo "  -h, --help         Afficher cette aide"
    echo ""
    echo "Exemples:"
    echo "  $0 video_folder                    # Joue avec délai par défaut (33ms)"
    echo "  $0 -d=50 video_folder              # Joue avec délai de 50ms (~20 FPS)"
    echo "  $0 --delay=16 video_folder          # Joue avec délai de 16ms (~62 FPS)"
    echo "  $0 -c video_folder                  # Joue depuis le cache RAM (/dev/shm)"
    echo "  $0 --cache video_folder -d=200      # Joue depuis cache avec délai de 200ms"
    echo ""
    echo "Structure attendue (dossier):"
    echo "  video_name/"
    echo "  ├── 1/"
    echo "  │   ├── b1_000.png"
    echo "  │   ├── b1_001.png"
    echo "  │   └── ..."
    echo "  ├── 2/"
    echo "  │   ├── b2_000.png"
    echo "  │   ├── b2_001.png"
    echo "  │   └── ..."
    echo "  └── ..."
    echo "  └── 14/"
    echo "      ├── b14_000.png"
    echo "      ├── b14_001.png"
    echo "      └── ..."
}

# Fonction pour parser les arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -d=*|--delay=*)
                DELAY_MS="${1#*=}"
                if ! [[ "$DELAY_MS" =~ ^[0-9]+$ ]] || [[ "$DELAY_MS" -lt 1 ]]; then
                    echo "Erreur: le délai doit être un entier positif" >&2
                    exit 1
                fi
                shift
                ;;
            -d|--delay)
                if [[ $# -lt 2 ]]; then
                    echo "Erreur: l'option $1 nécessite un argument" >&2
                    exit 1
                fi
                DELAY_MS="$2"
                if ! [[ "$DELAY_MS" =~ ^[0-9]+$ ]] || [[ "$DELAY_MS" -lt 1 ]]; then
                    echo "Erreur: le délai doit être un entier positif" >&2
                    exit 1
                fi
                shift 2
                ;;
            -c|--cache)
                CACHE_MODE=true
                shift
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            -*)
                echo "Erreur: option inconnue $1" >&2
                show_help >&2
                exit 1
                ;;
            *)
                if [[ -z "$FOLDER" ]]; then
                    if [[ -d "$1" ]]; then
                        FOLDER="$1"
                    else
                        echo "Erreur: $1 n'est pas un répertoire valide" >&2
                        show_help >&2
                        exit 1
                    fi
                else
                    echo "Erreur: un seul répertoire doit être spécifié" >&2
                    exit 1
                fi
                shift
                ;;
        esac
    done

    if [[ -z "$FOLDER" ]]; then
        echo "Erreur: répertoire requis" >&2
        show_help >&2
        exit 1
    fi

    if [[ -n "$FOLDER" ]]; then
        if [[ ! -d "$FOLDER" ]]; then
            echo "Erreur: le répertoire $FOLDER n'existe pas" >&2
            exit 1
        fi
    fi
}


# Fonction pour trouver le nombre maximum de frames (version rapide)
find_max_frames() {
    echo "Calcul du nombre de frames..."
    
    # Cas répertoire: compter les fichiers PNG directement
    local frame_count
    frame_count=$(find "$FOLDER" -name "*.png" | wc -l)
    
    if [[ "$frame_count" -eq 0 ]]; then
        echo "Erreur: aucun fichier PNG trouvé dans le répertoire" >&2
        exit 1
    fi
    
    # Calculer le numéro de frame maximum (en supposant 14 frames par frame vidéo)
    local max_frame=$((frame_count / 14 - 1))
    
    echo "✓ Nombre de frames détectées: $((max_frame + 1))"
    echo "$max_frame"
}

# Fonction pour se connecter au démon
connect_to_daemon() {
    if [[ ! -S "$SOCK_PATH" ]]; then
        echo "Erreur: socket du démon non trouvé: $SOCK_PATH" >&2
        echo "Assurez-vous que le démon ulanzi_d200_daemon est en cours d'exécution" >&2
        exit 1
    fi
    
    # Test de connexion
    if ! echo "ping" | timeout 2 nc -U "$SOCK_PATH" >/dev/null 2>&1; then
        echo "Erreur: impossible de se connecter au démon" >&2
        exit 1
    fi
    
    echo "✓ Connexion au démon établie"
}

# Fonction pour copier le dossier dans /dev/shm (cache)
setup_cache() {
    if [[ "$CACHE_MODE" != true ]]; then
        # Mode normal: utiliser directement le dossier
        MOUNT_POINT="$FOLDER"
        echo "✓ Utilisation directe du répertoire: $MOUNT_POINT"
        return
    fi
    
    echo "Configuration du cache RAM..."
    
    # Créer un nom unique pour le cache
    local cache_name
    cache_name="video_cache_$(basename "$FOLDER")_$$"
    CACHE_DIR="/dev/shm/$cache_name"
    
    # Créer le répertoire de cache
    if ! mkdir -p "$CACHE_DIR"; then
        echo "Erreur: impossible de créer le répertoire de cache $CACHE_DIR" >&2
        exit 1
    fi
    
    echo "Copie des fichiers dans le cache RAM..."
    
    # Copier récursivement le dossier avec barre de progression simple
    local total_files
    total_files=$(find "$FOLDER" -name "*.png" | wc -l)
    local copied_files=0
    
    # Utiliser rsync silencieux si disponible, sinon cp
    printf "Copie en cours..."
    if command -v rsync >/dev/null 2>&1; then
        rsync -a "$FOLDER/" "$CACHE_DIR/" 2>/dev/null || {
            echo "Erreur lors de la copie avec rsync, utilisation de cp..."
            cp -r "$FOLDER"/* "$CACHE_DIR/"
        }
    else
        cp -r "$FOLDER"/* "$CACHE_DIR/"
    fi
    
    printf " Terminé!\n"
    
    # Vérifier que la copie a réussi
    local cached_files
    cached_files=$(find "$CACHE_DIR" -name "*.png" | wc -l)
    
    if [[ "$cached_files" -eq "$total_files" ]]; then
        echo "✓ Cache créé: $cached_files/$total_files fichiers copiés"
    else
        echo "Avertissement: $cached_files/$total_files fichiers copiés" >&2
    fi
    
    # Utiliser le cache comme point de montage
    MOUNT_POINT="$CACHE_DIR"
    echo "✓ Utilisation du cache: $MOUNT_POINT"
}

# Fonction pour nettoyer le cache
cleanup_cache() {
    if [[ "$CACHE_MODE" == true ]] && [[ -n "$CACHE_DIR" ]] && [[ -d "$CACHE_DIR" ]]; then
        echo "Nettoyage du cache..."
        rm -rf "$CACHE_DIR"
        echo "✓ Cache supprimé"
    fi
}

# Gestionnaire de signal pour nettoyer le cache en cas d'interruption
signal_handler() {
    echo -e "\nInterruption détectée..."
    cleanup_cache
    cleanup_socket
    cleanup_state
    exit 0
}

# Fonctions de gestion d'état partagé
init_state() {
    mkdir -p "$STATE_DIR"
    echo "0" > "$CURRENT_FRAME_FILE"
    echo "true" > "$PLAYING_FILE"
    echo "false" > "$PAUSED_FILE"
    echo $$ > "$MAIN_PID_FILE"  # Sauvegarder le PID du processus principal
}

get_main_pid() {
    cat "$MAIN_PID_FILE" 2>/dev/null
}

get_current_frame() {
    cat "$CURRENT_FRAME_FILE" 2>/dev/null || echo "0"
}

set_current_frame() {
    echo "$1" > "$CURRENT_FRAME_FILE"
}

is_playing() {
    [[ "$(cat "$PLAYING_FILE" 2>/dev/null)" == "true" ]]
}

set_playing() {
    echo "$1" > "$PLAYING_FILE"
}

is_paused() {
    [[ "$(cat "$PAUSED_FILE" 2>/dev/null)" == "true" ]]
}

set_paused() {
    echo "$1" > "$PAUSED_FILE"
}

cleanup_state() {
    rm -rf "$STATE_DIR"
}

# Fonctions de gestion du socket de contrôle
cleanup_socket() {
    rm -f "$CONTROL_SOCKET" 2>/dev/null || true
}

setup_control_socket() {
    # Nettoyer l'ancien socket s'il existe
    cleanup_socket

    
    # Créer le socket UNIX avec nc en arrière-plan
    nc -l -U "$CONTROL_SOCKET" >/dev/null 2>&1 &
    local nc_pid=$!
    
    # Vérifier que nc a démarré
    sleep 0.1
    if ! kill -0 $nc_pid 2>/dev/null; then
        echo "Erreur: impossible de créer le socket de contrôle $CONTROL_SOCKET" >&2
        return 1
    fi
    
    echo "✓ Socket de contrôle créé: $CONTROL_SOCKET"
    return 0
}

# Fonctions de contrôle de lecture
send_command() {
    local cmd="$1"
    echo "$cmd" | nc -w 1 -U "$CONTROL_SOCKET" 2>/dev/null || echo "Erreur: impossible d'envoyer la commande '$cmd'" >&2
}

handle_command() {
    local cmd="$1"
    
    case "$cmd" in
        "play")
            set_playing "true"
            set_paused "false"
            echo "▶ Lecture"
            ;;
        "pause")
            set_playing "false"
            set_paused "true"
            echo "⏸ Pause"
            ;;
        "stop")
            echo "⏹ Arrêt du démon (CTRL+C)"
            # Envoyer SIGINT (CTRL+C) au processus principal
            local main_pid=$(get_main_pid)
            if [[ -n "$main_pid" ]]; then
                kill -INT "$main_pid" 2>/dev/null || true
            else
                echo "Erreur: impossible de trouver le PID du processus principal" >&2
            fi
            ;;
        "back")
            local current=$(get_current_frame)
            local new_frame=$((current - 75))
            if [[ $new_frame -lt 0 ]]; then
                new_frame=0
            fi
            set_current_frame "$new_frame"
            echo "⏪ Recul: frame $new_frame/$TOTAL_FRAMES"
            ;;
        "forward")
            local current=$(get_current_frame)
            local new_frame=$((current + 75))
            if [[ $new_frame -ge $TOTAL_FRAMES ]]; then
                new_frame=$((TOTAL_FRAMES - 1))
            fi
            set_current_frame "$new_frame"
            echo "⏩ Avance: frame $new_frame/$TOTAL_FRAMES"
            ;;
        "status")
            local status="Arrêté"
            if is_playing; then
                status="Lecture"
            elif is_paused; then
                status="Pause"
            fi
            local current=$(get_current_frame)
            echo "État: $status | Frame: $current/$TOTAL_FRAMES"
            ;;
        *)
            echo "Commande inconnue: $cmd"
            echo "Commandes disponibles: play, pause, stop, back, forward, status"
            ;;
    esac
}

# Fonction d'écoute des commandes (en arrière-plan)
listen_commands() {
    while true; do
        if command -v nc >/dev/null 2>&1; then
            nc -l -U "$CONTROL_SOCKET" 2>/dev/null | while read -r cmd; do
                handle_command "$cmd"
            done
        else
            echo "Erreur: nc (netcat) n'est pas disponible" >&2
            break
        fi
        sleep 0.1
    done
}

# Fonction pour déterminer le format de padding selon le nombre de frames
get_frame_format() {
    local max_frames="$1"
    
    if [[ $max_frames -lt 10 ]]; then
        echo "%d"           # 0-9
    elif [[ $max_frames -lt 100 ]]; then
        echo "%02d"          # 00-99
    elif [[ $max_frames -lt 1000 ]]; then
        echo "%03d"          # 000-999
    elif [[ $max_frames -lt 10000 ]]; then
        echo "%04d"          # 0000-9999
    elif [[ $max_frames -lt 100000 ]]; then
        echo "%05d"          # 00000-99999
    elif [[ $max_frames -lt 1000000 ]]; then
        echo "%06d"          # 000000-999999
    elif [[ $max_frames -lt 10000000 ]]; then
        echo "%07d"          # 0000000-9999999
    elif [[ $max_frames -lt 100000000 ]]; then
        echo "%08d"          # 00000000-99999999
    elif [[ $max_frames -lt 1000000000 ]]; then
        echo "%09d"          # 000000000-999999999
    elif [[ $max_frames -lt 10000000000 ]]; then
        echo "%10d"          # 0000000000-9999999999
    elif [[ $max_frames -lt 100000000000 ]]; then
        echo "%11d"          # 00000000000-99999999999
    elif [[ $max_frames -lt 1000000000000 ]]; then
        echo "%12d"          # 000000000000-999999999999
    else
        echo "%13d"          # 0000000000000+ (milliards+)
    fi
}

# Fonction pour envoyer une frame spécifique au device
send_frame() {
    local frame_num="$1"
    send_frame_from_mount "$frame_num" "$TOTAL_FRAMES"
}

# Fonction pour envoyer une frame depuis l'archive montée
send_frame_from_mount() {
    local frame_num="$1"
    local max_frames="$2"
    
    # Obtenir le format de padding dynamique
    local frame_format
    frame_format=$(get_frame_format $max_frames)
    
    # Construire la commande set-buttons-explicit-14 avec les chemins directs (nouveau format bX_YYY)
    local cmd="set-buttons-explicit-14"
    cmd+=" --button-1=$MOUNT_POINT/1/b1_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-2=$MOUNT_POINT/2/b2_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-3=$MOUNT_POINT/3/b3_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-4=$MOUNT_POINT/4/b4_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-5=$MOUNT_POINT/5/b5_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-6=$MOUNT_POINT/6/b6_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-7=$MOUNT_POINT/7/b7_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-8=$MOUNT_POINT/8/b8_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-9=$MOUNT_POINT/9/b9_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-10=$MOUNT_POINT/10/b10_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-11=$MOUNT_POINT/11/b11_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-12=$MOUNT_POINT/12/b12_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-13=$MOUNT_POINT/13/b13_$(printf "$frame_format" $frame_num).png"
    cmd+=" --button-14=$MOUNT_POINT/14/b14_$(printf "$frame_format" $frame_num).png"
    
    # Envoyer la commande au démon via socket
    printf '%s\n' "$cmd" | nc -w 1 -U "$SOCK_PATH" >/dev/null || {
        echo "Erreur: impossible d'envoyer la commande au démon" >&2
        return 1
    }
}

# Fonction principale de lecture
play_video() {
    local max_frame="$1"
    local current_frame=0
    
    echo "Début de la lecture (délai: ${DELAY_MS}ms, FPS: $(echo "scale=2; 1000/$DELAY_MS" | bc -l))"
    echo "Appuyez sur Ctrl+C pour arrêter"
    
    # Gestion du signal pour arrêt propre et démontage
    trap 'echo -e "\nArrêt demandé..."; unmount_archive; exit 0' INT TERM
    
    while [[ $current_frame -le $max_frame ]]; do
        echo -ne "\rFrame: $current_frame/$max_frame"
        
        send_frame_from_mount "$current_frame" "$max_frame"
        
        # Attendre le délai
        sleep $(echo "scale=3; $DELAY_MS/1000" | bc -l)
        current_frame=$((current_frame + 1))
    done
    
    echo -e "\nLecture terminée"
}

# Fonction principale
main() {
    echo "Lecteur de vidéo rendue - $(basename "$0")"
    echo "=================================="
    
    # Nettoyer les anciens points de montage au cas où
    local old_mount_points
    old_mount_points=$(find /dev/shm -name "tmpvideoplayback_*" -type d 2>/dev/null || true)
    if [[ -n "$old_mount_points" ]]; then
        echo "Nettoyage des anciens points de montage..."
        echo "$old_mount_points" | while read -r mount_point; do
            if mountpoint -q "$mount_point" 2>/dev/null; then
                umount "$mount_point" 2>/dev/null || true
            fi
            rm -rf "$mount_point"
        done
    fi
    
    parse_args "$@"
    
    # Enregistrer le gestionnaire de signal pour CTRL+C
    trap signal_handler INT TERM
    
    # Obtenir le nombre total de frames
    TOTAL_FRAMES=$(find_max_frames | tail -1)
    
    # Configuration du cache
    setup_cache
    
    # Démarrer le socket de contrôle en arrière-plan
    setup_control_socket
    listen_commands &
    LISTENER_PID=$!
    
    # Initialiser l'état
    init_state
    
    echo "🎬 Démarrage de la lecture (frames: $TOTAL_FRAMES)"
    echo "📡 Socket de contrôle: $CONTROL_SOCKET"
    echo "⌨️  Commandes: play, pause, stop, back, forward, status"
    
    # Boucle principale de lecture
    while true; do
        local current_frame=$(get_current_frame)
        
        # Vérifier si on a atteint la fin
        if [[ $current_frame -ge $TOTAL_FRAMES ]]; then
            echo -e "\n🏁 Fin de la vidéo"
            break
        fi
        
        if is_playing; then
            # Envoyer la frame actuelle au device
            send_frame "$current_frame"
            
            # Afficher la progression
            printf "\r🎬 Frame: %d/%d (%.1f%%)" $current_frame $TOTAL_FRAMES $((current_frame * 100 / TOTAL_FRAMES))
            
            # Passer à la frame suivante
            set_current_frame $((current_frame + 1))
            
            # Délai entre frames
            sleep $(echo "scale=3; $DELAY_MS/1000" | bc -l)
        else
            # En pause, attendre un peu
            sleep 0.1
        fi
    done
    
    # Fin de la vidéo
    set_playing "false"
    
    # Nettoyage
    cleanup_cache
    cleanup_socket
    cleanup_state
    kill $LISTENER_PID 2>/dev/null || true
    
    echo "✅ Lecteur arrêté"
}

# Script utilitaire pour contrôler le démon
if [[ "$(basename "$0")" == "play_rendered_video_control" ]]; then
    # Si appelé comme controlleur
    case "${1:-}" in
        "play"|"pause"|"stop"|"back"|"forward"|"status")
            send_command "$1"
            ;;
        *)
            echo "Usage: $0 <command>"
            echo "Commandes: play, pause, stop, back, forward, status"
            exit 1
            ;;
    esac
    exit 0
fi

# Exécuter la fonction principale
main "$@"
