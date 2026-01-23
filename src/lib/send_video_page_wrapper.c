#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <libgen.h>
#include <png.h>
#include <zlib.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>

#include <signal.h>
#include <stdatomic.h>
// #include <zip.h>  // Pas disponible, utilisation de la commande système
#include <dirent.h>  // Pour readdir/opendir

// Variables globales pour la gestion du signal
static volatile sig_atomic_t stop_requested = 0;

static void die_snprintf(const char *label) {
    fprintf(stderr, "Erreur: buffer trop petit (snprintf) pour %s\n", label);
    exit(1);
}

static int snprintf_checked(char *dst, size_t cap, const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) die_snprintf(label);
    return n;
}

static char *asprintf_alloc(const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) die_snprintf(label);
    size_t sz = (size_t)n + 1;
    char *buf = (char *)malloc(sz);
    if (!buf) {
        fprintf(stderr, "Erreur: malloc(%zu) échoué pour %s\n", sz, label);
        exit(1);
    }
    va_start(ap, fmt);
    int n2 = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    if (n2 < 0 || (size_t)n2 >= sz) die_snprintf(label);
    return buf;
}

// Gestionnaire de signal pour CTRL+C et SIGTERM
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nSignal %d détecté, arrêt en cours...\n", sig);
        stop_requested = 1;
    }
}

// Fonction pour générer le préfixe de frame avec zéros padding et préfixe de bouton
static void format_frame_prefix(char *prefix, int frame_num, int total_frames, int button_num) {
    // Ajouter le préfixe de bouton b1-b14
    if (total_frames < 10) {
        snprintf(prefix, 32, "b%d_%d", button_num, frame_num);
    } else if (total_frames < 100) {
        snprintf(prefix, 32, "b%d_%02d", button_num, frame_num);
    } else if (total_frames < 1000) {
        snprintf(prefix, 32, "b%d_%03d", button_num, frame_num);
    } else {
        snprintf(prefix, 32, "b%d_%04d", button_num, frame_num);
    }
}

// Options de traitement
typedef struct {
    int max_frames;
    int magnify_size;     // Taille de magnification (-m/--magnify, 0 = désactivé)
    int quality_size;     // Taille de qualité (-q/--quality, 0 = pas de redimensionnement)
    int render_mode;      // Mode render (-r/--render, 0 = désactivé)
    int dither_mode;     // Mode dithering (-d/--dither, 0 = désactivé)
    int sleep_delay;      // Délai entre frames (-s/--sleep, 0 = pas de délai)
    char *convert_opts;   // Options de conversion (-c/--convert, NULL = désactivé)
} process_options_t;

// Fonction pour écrire une frame en PNG
static int write_frame_png(const char *path, const uint8_t *rgba_data, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); fclose(fp); return -1; }
    
    png_init_io(png, fp);
    png_set_compression_level(png, Z_BEST_SPEED);
    png_set_filter(png, 0, PNG_ALL_FILTERS);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    
    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    if (!rows) { png_destroy_write_struct(&png, &info); fclose(fp); return -1; }
    
    for (int y = 0; y < h; y++) {
        rows[y] = (png_bytep)(rgba_data + (size_t)y * w * 4);
    }
    
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    
    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

// Fonction pour extraire une frame de la vidéo
static uint8_t* extract_frame(AVFormatContext *fmt_ctx, AVCodecContext *codec_ctx, 
                           AVFrame *frame, AVPacket *pkt, int stream_idx, 
                           int target_w, int target_h) {
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        // Vérifier le signal pendant la lecture des packets
        if (stop_requested) {
            av_packet_unref(pkt);
            return NULL;
        }
        
        if (pkt->stream_index == stream_idx) {
            int ret = avcodec_send_packet(codec_ctx, pkt);
            if (ret < 0) {
                av_packet_unref(pkt);
                continue;
            }
            
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == 0) {
                // Convertir en RGBA
                struct SwsContext *sws_ctx = sws_getContext(
                    frame->width, frame->height, codec_ctx->pix_fmt,
                    target_w, target_h, AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, NULL, NULL, NULL);
                
                if (!sws_ctx) {
                    av_packet_unref(pkt);
                    return NULL;
                }
                
                uint8_t *rgba_data = malloc((size_t)target_w * target_h * 4);
                if (!rgba_data) {
                    sws_freeContext(sws_ctx);
                    av_packet_unref(pkt);
                    return NULL;
                }
                
                uint8_t *dst_planes[4] = { rgba_data, NULL, NULL, NULL };
                int dst_strides[4] = { target_w * 4, 0, 0, 0 };
                
                sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                         0, frame->height, dst_planes, dst_strides);
                
                sws_freeContext(sws_ctx);
                av_packet_unref(pkt);
                return rgba_data;
            }
        }
        av_packet_unref(pkt);
    }
    return NULL;
}

// Fonction principale
int main(int argc, char **argv) {
    // Initialiser les options par défaut
    process_options_t opts = {
        .max_frames = 0,
        .magnify_size = 0,   // Défaut: magnification désactivée
        .quality_size = 0,   // Défaut: pas de redimensionnement
        .render_mode = 0,    // Défaut: mode render désactivé
        .dither_mode = 0,    // Défaut: dithering désactivé
        .sleep_delay = 0,    // Défaut: pas de délai
        .convert_opts = NULL  // Défaut: pas de conversion
    };
    
    const char *video_path = NULL;
    
    // Parser les arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options] <video_file>\n", argv[0]);
            printf("\nOptions:\n");
            printf("  --max-frames=N         Nombre maximum de frames à traiter\n");
            printf("  -m, --magnify=N      Magnifier les icônes (16-196, défaut: désactivé)\n");
            printf("  -q, --quality=N      Redimensionner les icônes (16-196, défaut: 196)\n");
            printf("  -r, --render         Mode render: génère des icônes par frame dans des dossiers\n");
            printf("  -d, --dither         Activer le dithering Floyd-Steinberg\n");
            printf("  -s, --sleep=MS      Délai entre chaque frame en millisecondes (défaut: 33)\n");
            printf("  -c, --convert=OPTS  Convertir la vidéo avant traitement (passe options à convert_video.sh)\n");
            printf("  -h, --help            Afficher cette aide\n");
            printf("\nExemples:\n");
            printf("  %s video.mp4                                    # Comportement par défaut\n", argv[0]);
            printf("  %s --max-frames=30 video.mp4                    # Traiter 30 frames maximum\n", argv[0]);
            printf("  %s -m=128 video.mp4                             # Magnifier les icônes en 128x128\n", argv[0]);
            printf("  %s -q=64 video.mp4                              # Redimensionner en 64x64\n", argv[0]);
            printf("  %s --max-frames=10 -m=196 video.mp4            # 10 frames avec icônes 196x196\n", argv[0]);
            printf("  %s -r video.mp4                                 # Mode render: génère des dossiers par bouton\n", argv[0]);
            printf("  %s -r --max-frames=5 video.mp4                  # Render avec 5 frames seulement\n", argv[0]);
            printf("  %s -c=\"--size=720 --fps=30\" video.mp4           # Convertir en 720p 30fps puis traiter\n", argv[0]);
            printf("  %s --convert=\"--size=360\" video.mp4               # Convertir en 360p puis traiter\n", argv[0]);
            printf("\nMode render (-r/--render):\n");
            printf("  Crée une structure de dossiers: <video_name>/<button_number>/\n");
            printf("  Génère des icônes pour chaque frame avec préfixe numéroté (000, 001, ...)\n");
            printf("  Exemple: video.mp4/1/000.png, video.mp4/1/001.png, ..., video.mp4/14/999.png\n");
            return 0;
        } else if (strncmp(argv[i], "--max-frames=", 12) == 0) {
            opts.max_frames = atoi(argv[i] + 12);
        } else if (strncmp(argv[i], "-m=", 3) == 0) {
            opts.magnify_size = atoi(argv[i] + 3);
            if (opts.magnify_size < 16 || opts.magnify_size > 196) {
                fprintf(stderr, "Erreur: taille de magnification doit être entre 16 et 196\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--magnify=", 10) == 0) {
            opts.magnify_size = atoi(argv[i] + 10);
            if (opts.magnify_size < 16 || opts.magnify_size > 196) {
                fprintf(stderr, "Erreur: taille de magnification doit être entre 16 et 196\n");
                return 1;
            }
        } else if (strncmp(argv[i], "-q=", 3) == 0) {
            opts.quality_size = atoi(argv[i] + 3);
            if (opts.quality_size < 16 || opts.quality_size > 196) {
                fprintf(stderr, "Erreur: taille de quality doit être entre 16 et 196\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--quality=", 10) == 0) {
            opts.quality_size = atoi(argv[i] + 10);
            if (opts.quality_size < 16 || opts.quality_size > 196) {
                fprintf(stderr, "Erreur: taille de quality doit être entre 16 et 196\n");
                return 1;
            }
        } else if (strncmp(argv[i], "-c=", 3) == 0) {
            opts.convert_opts = argv[i] + 3;
            if (strlen(opts.convert_opts) == 0) {
                fprintf(stderr, "Erreur: options de conversion requises pour -c=\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--convert=", 10) == 0) {
            opts.convert_opts = argv[i] + 10;
            if (strlen(opts.convert_opts) == 0) {
                fprintf(stderr, "Erreur: options de conversion requises pour --convert=\n");
                return 1;
            }
        } else if (strncmp(argv[i], "-s=", 3) == 0) {
            opts.sleep_delay = atoi(argv[i] + 3);
            if (opts.sleep_delay < 1) {
                fprintf(stderr, "Erreur: le délai doit être un entier positif\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--sleep=", 8) == 0) {
            opts.sleep_delay = atoi(argv[i] + 8);
            if (opts.sleep_delay < 1) {
                fprintf(stderr, "Erreur: le délai doit être un entier positif\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--render") == 0) {
            opts.render_mode = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dither") == 0) {
            opts.dither_mode = 1;
        } else if (strncmp(argv[i], "--", 2) == 0) {
            fprintf(stderr, "Erreur: option inconnue %s\n", argv[i]);
            return 1;
        } else {
            // Traiter comme nom de fichier vidéo
            if (video_path == NULL) {
                video_path = argv[i];
            } else {
                fprintf(stderr, "Erreur: une seule vidéo doit être spécifiée\n");
                return 1;
            }
        }
    }
    
    // Obtenir le répertoire de travail et construire les chemins
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Erreur: impossible d'obtenir le répertoire courant\n");
        return 1;
    }
    
    // Construire le chemin absolu vers send_image_page
    char send_image_page_path[PATH_MAX];
    snprintf_checked(send_image_page_path, sizeof(send_image_page_path), "send_image_page_path",
                     "%s/lib/send_image_page", cwd);
    
    if (!video_path) {
        fprintf(stderr, "Erreur: fichier vidéo requis\n");
        return 1;
    }
    
    // Gérer la conversion si demandée
    char actual_video_path[PATH_MAX];
    if (opts.convert_opts) {
        printf("Conversion de la vidéo avant traitement...\n");
        
        // Construire la commande de conversion
        char *convert_cmd = asprintf_alloc("convert_cmd", "%s/lib/convert_video.sh %s \"%s\"",
                                           cwd, opts.convert_opts, video_path);
        
        printf("Commande: %s\n", convert_cmd);
        
        // Exécuter la conversion
        int convert_result = system(convert_cmd);
        free(convert_cmd);
        if (convert_result != 0) {
            fprintf(stderr, "Erreur: échec de la conversion vidéo (code: %d)\n", convert_result);
            return 1;
        }
        
        // Extraire le nom du fichier converti selon la logique de convert_video.sh
        char *video_name = strdup(video_path);
        char *last_slash = strrchr(video_name, '/');
        char *video_dir = NULL;
        
        if (last_slash) {
            *last_slash = '\0';  // Séparer le chemin du nom
            video_dir = video_name;
            video_name = last_slash + 1;
        } else {
            video_dir = ".";
        }
        
        char *dot = strrchr(video_name, '.');
        if (dot) *dot = '\0';
        
        // Déterminer le nom du fichier converti et le chemin complet
        if (strstr(opts.convert_opts, "--size=") && strstr(opts.convert_opts, "--fps=")) {
            // size + fps: nom_converted_sizep_fpsfps.mp4
            char size_str[10], fps_str[10];
            sscanf(strstr(opts.convert_opts, "--size=") + 7, "%[^ ]", size_str);
            sscanf(strstr(opts.convert_opts, "--fps=") + 6, "%s", fps_str);
            snprintf(actual_video_path, sizeof(actual_video_path), "%s/%s_converted_%sp_%sfps.mp4", 
                     video_dir, video_name, size_str, fps_str);
        } else if (strstr(opts.convert_opts, "--size=")) {
            // size seul: nom_converted_sizep.mp4
            char size_str[10];
            sscanf(strstr(opts.convert_opts, "--size=") + 7, "%[^ ]", size_str);
            snprintf(actual_video_path, sizeof(actual_video_path), "%s/%s_converted_%sp.mp4", 
                     video_dir, video_name, size_str);
        } else if (strstr(opts.convert_opts, "--fps=")) {
            // fps seul: nom_converted_fpsfps.mp4
            char fps_str[10];
            sscanf(strstr(opts.convert_opts, "--fps=") + 6, "%s", fps_str);
            snprintf(actual_video_path, sizeof(actual_video_path), "%s/%s_converted_%sfps.mp4", 
                     video_dir, video_name, fps_str);
        } else {
            // sans options: nom_converted.mp4
            snprintf(actual_video_path, sizeof(actual_video_path), "%s/%s_converted.mp4", 
                     video_dir, video_name);
        }
        
        printf("Vidéo convertie: %s\n", actual_video_path);
        video_path = actual_video_path;
    } else {
        strncpy(actual_video_path, video_path, sizeof(actual_video_path) - 1);
        actual_video_path[sizeof(actual_video_path) - 1] = '\0';
    }
    
    // Ouvrir la vidéo
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, video_path, NULL, NULL) < 0) {
        fprintf(stderr, "Erreur: impossible d'ouvrir la vidéo %s\n", video_path);
        return 1;
    }
    
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Erreur: impossible d'obtenir les infos de la vidéo\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    // Trouver le stream vidéo
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    if (video_stream_idx == -1) {
        fprintf(stderr, "Erreur: aucun stream vidéo trouvé\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    // Initialiser le codec
    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Erreur: codec non trouvé\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Erreur: impossible d'allouer le contexte codec\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Erreur: impossible de copier les paramètres codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Erreur: impossible d'ouvrir le codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    printf("Traitement de la vidéo: %s\n", video_path);
    printf("Résolution: %dx%d -> 1280x720\n", codec_ctx->width, codec_ctx->height);
    if (opts.max_frames > 0) {
        printf("Frames maximum: %d\n", opts.max_frames);
    }
    if (opts.magnify_size > 0) {
        printf("Magnification: %dx%d\n", opts.magnify_size, opts.magnify_size);
    }
    if (opts.quality_size > 0) {
        printf("Quality: %dx%d\n", opts.quality_size, opts.quality_size);
    }
    if (opts.render_mode) {
        printf("Mode render: activé\n");
    }
    
    // Créer le répertoire temporaire dans /dev/shm/
    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "/dev/shm/video_render_%ld", (long)getpid());
    if (mkdir(tmpdir, 0755) != 0) {
        fprintf(stderr, "Erreur: impossible de créer le répertoire temporaire %s\n", tmpdir);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    
    // En mode render, créer le dossier de la vidéo dans /dev/shm/
    char video_dir[PATH_MAX];
    char final_video_dir[PATH_MAX];  // Pour le dossier final
    if (opts.render_mode) {
        // Créer le dossier temporaire dans /dev/shm/
        snprintf_checked(video_dir, sizeof(video_dir), "video_dir", "%s/render_output", tmpdir);
        
        // Extraire le nom de la vidéo pour le dossier final
        const char *video_basename = strrchr(video_path, '/');
        if (!video_basename) video_basename = video_path;
        else video_basename++;
        
        // Copier le nom et enlever l'extension pour le dossier final
        char video_name[256];
        strncpy(video_name, video_basename, sizeof(video_name) - 1);
        video_name[sizeof(video_name) - 1] = '\0';
        char *dot = strrchr(video_name, '.');
        if (dot) *dot = '\0';
        
        // Chemin du répertoire contenant la vidéo pour le dossier final
        char video_dir_path[PATH_MAX];
        char abs_video_path[PATH_MAX];
        
        // Obtenir le chemin absolu du fichier vidéo
        if (video_path[0] == '/') {
            // Déjà absolu
            strncpy(abs_video_path, video_path, sizeof(abs_video_path) - 1);
        } else {
            // Relatif, convertir en absolu
            char cwd2[PATH_MAX];
            if (!getcwd(cwd2, sizeof(cwd2))) {
                fprintf(stderr, "Erreur: impossible d'obtenir le répertoire courant\n");
                return 1;
            }
            snprintf_checked(abs_video_path, sizeof(abs_video_path), "abs_video_path", "%s/%s", cwd2, video_path);
        }
        abs_video_path[sizeof(abs_video_path) - 1] = '\0';
        
        strncpy(video_dir_path, abs_video_path, sizeof(video_dir_path) - 1);
        video_dir_path[sizeof(video_dir_path) - 1] = '\0';
        
        char *last_slash = strrchr(video_dir_path, '/');
        if (last_slash) {
            *last_slash = '\0';
        } else {
            // Pas de chemin, utiliser le répertoire courant absolu
            if (!getcwd(video_dir_path, sizeof(video_dir_path))) {
                fprintf(stderr, "Erreur: impossible d'obtenir le répertoire courant\n");
                return 1;
            }
        }
        
        // Construire le chemin du dossier final (absolu)
        snprintf_checked(final_video_dir, sizeof(final_video_dir), "final_video_dir", "%s/%s", video_dir_path, video_name);
        
        // Créer le répertoire de render temporaire
        if (mkdir(video_dir, 0755) != 0) {
            fprintf(stderr, "Erreur: impossible de créer le répertoire %s\n", video_dir);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return 1;
        }
        // printf("Répertoire de render temporaire: %s\n", video_dir);
        printf("Dossier final sera créé dans: %s\n", final_video_dir);
    }
    
    // printf("Répertoire temporaire: %s\n", tmpdir);
    
    // Enregistrer le gestionnaire de signal pour CTRL+C et SIGTERM
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Variables pour le traitement
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int frame_count = 0;
    
    // En mode render, on doit d'abord compter toutes les frames
    int total_frames = 0;
    if (opts.render_mode) {
        printf("Calcul du nombre total de frames...\n");
        
        // Réinitialiser la lecture de la vidéo
        av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec_ctx);
        
        // Compter les frames
        while (!stop_requested) {
            uint8_t *rgba_data = extract_frame(fmt_ctx, codec_ctx, frame, pkt, 
                                           video_stream_idx, codec_ctx->width, codec_ctx->height);
            if (!rgba_data) break;
            
            free(rgba_data);
            total_frames++;
            
            if (opts.max_frames > 0 && total_frames >= opts.max_frames) break;
        }
        
        printf("Nombre total de frames: %d\n", total_frames);
        
        // Réinitialiser à nouveau pour le traitement
        av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec_ctx);
    }
    
    // Boucle de traitement des frames
    struct timeval start_time, current_time;
    gettimeofday(&start_time, NULL);
    
    while (1) {
        // Vérifier si CTRL+C a été pressé (vérification plus fréquente)
        if (stop_requested) {
            printf("Arrêt demandé par l'utilisateur.\n");
            break;
        }
        
        if (opts.max_frames > 0 && frame_count >= opts.max_frames) {
            break;
        }
        
        uint8_t *rgba_data = extract_frame(fmt_ctx, codec_ctx, frame, pkt, 
                                       video_stream_idx, codec_ctx->width, codec_ctx->height);
        if (!rgba_data) {
            break;
        }
        
        // Vérifier à nouveau le signal après extraction (car FFmpeg peut bloquer)
        if (stop_requested) {
            free(rgba_data);
            printf("Arrêt demandé pendant l'extraction.\n");
            break;
        }
        
        // Créer le fichier PNG pour cette frame
        char frame_png[PATH_MAX];
        snprintf_checked(frame_png, sizeof(frame_png), "frame_png", "%s/frame_%06d.png", tmpdir, frame_count);
        
        if (write_frame_png(frame_png, rgba_data, codec_ctx->width, codec_ctx->height) == 0) {
            // printf("Frame %d: %s (%dx%d)\n", frame_count + 1, frame_png, codec_ctx->width, codec_ctx->height);
            
            // Vérifier le signal AVANT d'exécuter la commande
            if (stop_requested) {
                printf("Arrêt demandé avant l'exécution de la commande.\n");
                unlink(frame_png);
                free(rgba_data);
                break;
            }
            
            // Construire et exécuter la commande send_image_page
            char cmd[8192];
            int cmd_len = 0;
            
            // Base de la commande
            cmd_len += snprintf_checked(cmd + cmd_len, sizeof(cmd) - (size_t)cmd_len, "cmd_base",
                                        "%s -o --no-tile-optimize", send_image_page_path);
            
            // Ajouter l'option dithering si spécifiée
            if (opts.dither_mode) {
                cmd_len += snprintf_checked(cmd + cmd_len, sizeof(cmd) - (size_t)cmd_len, "cmd_dither", " -d");
            }
            
            // Ajouter l'option magnify si spécifiée
            if (opts.magnify_size > 0) {
                cmd_len += snprintf_checked(cmd + cmd_len, sizeof(cmd) - (size_t)cmd_len, "cmd_magnify",
                                            " -m=%d", opts.magnify_size);
            }
            
            // Ajouter l'option quality si spécifiée
            if (opts.quality_size > 0) {
                cmd_len += snprintf_checked(cmd + cmd_len, sizeof(cmd) - (size_t)cmd_len, "cmd_quality",
                                            " -q=%d", opts.quality_size);
            }
            
            // Ajouter le fichier image avec chemin absolu
            char abs_frame_png[PATH_MAX];
            if (realpath(frame_png, abs_frame_png) == NULL) {
                strcpy(abs_frame_png, frame_png);  // Fallback au chemin relatif
            }
            cmd_len += snprintf_checked(cmd + cmd_len, sizeof(cmd) - (size_t)cmd_len, "cmd_frame",
                                        " \"%s\"", abs_frame_png);
            
            // En mode render, ajouter l'option -k pour copier les icônes
            if (opts.render_mode) {
                // Créer un dossier temporaire pour cette frame
                char frame_temp_dir[PATH_MAX];
                snprintf_checked(frame_temp_dir, sizeof(frame_temp_dir), "frame_temp_dir", "%s/frame_%03d", video_dir, frame_count);
                char mkdir_cmd[PATH_MAX];
                snprintf_checked(mkdir_cmd, sizeof(mkdir_cmd), "mkdir_cmd", "mkdir -p \"%s\"", frame_temp_dir);
                (void)system(mkdir_cmd);
                
                // Construire la commande send_image_page avec -k pour cette frame
                char render_cmd[8192];
                int render_cmd_len = 0;
                
                render_cmd_len += snprintf_checked(render_cmd + render_cmd_len, sizeof(render_cmd) - (size_t)render_cmd_len,
                                                   "render_base", "%s -o --no-tile-optimize", send_image_page_path);
                
                // Ajouter l'option dithering si spécifiée
                if (opts.dither_mode) {
                    render_cmd_len += snprintf_checked(render_cmd + render_cmd_len, sizeof(render_cmd) - (size_t)render_cmd_len,
                                                       "render_dither", " -d");
                }
                
                if (opts.magnify_size > 0) {
                    render_cmd_len += snprintf_checked(render_cmd + render_cmd_len, sizeof(render_cmd) - (size_t)render_cmd_len,
                                                       "render_magnify", " -m=%d", opts.magnify_size);
                }
                
                if (opts.quality_size > 0) {
                    render_cmd_len += snprintf_checked(render_cmd + render_cmd_len, sizeof(render_cmd) - (size_t)render_cmd_len,
                                                       "render_quality", " -q=%d", opts.quality_size);
                }
                
                // Ajouter l'option -k avec le dossier temporaire de cette frame
                render_cmd_len += snprintf_checked(render_cmd + render_cmd_len, sizeof(render_cmd) - (size_t)render_cmd_len,
                                                   "render_k", " -k=\"%s\"=\"%s\"", frame_temp_dir, "icon");
                
                // Ajouter le fichier image avec chemin absolu
                char abs_frame_png[PATH_MAX];
                if (realpath(frame_png, abs_frame_png) == NULL) {
                    strcpy(abs_frame_png, frame_png);  // Fallback au chemin relatif
                }
                render_cmd_len += snprintf_checked(render_cmd + render_cmd_len, sizeof(render_cmd) - (size_t)render_cmd_len,
                                                   "render_frame", " \"%s\"", abs_frame_png);
                
                // Exécuter la commande pour cette frame
                int render_result = system(render_cmd);
                
                 if (render_result == 0) {
                    // Déplacer les icônes dans les bons dossiers de boutons
                    for (int button = 1; button <= 14; button++) {
                        char button_dir[PATH_MAX];
                        snprintf_checked(button_dir, sizeof(button_dir), "button_dir", "%s/%d", video_dir, button);
                        
                        // Créer le dossier du bouton s'il n'existe pas
                        char mkdir_button_cmd[PATH_MAX];
                        snprintf_checked(mkdir_button_cmd, sizeof(mkdir_button_cmd), "mkdir_button_cmd", "mkdir -p \"%s\"", button_dir);
                        (void)system(mkdir_button_cmd);
                        
                        // Déplacer l'icône du bouton correspondant
                        char src_icon[PATH_MAX];
                        char dst_icon[PATH_MAX];
                        snprintf_checked(src_icon, sizeof(src_icon), "src_icon", "%s/icon-%d.png", frame_temp_dir, button);
                        
                        // Utiliser le nouveau format avec préfixe de bouton
                        char button_frame_prefix[32];
                        format_frame_prefix(button_frame_prefix, frame_count, total_frames, button);
                        snprintf_checked(dst_icon, sizeof(dst_icon), "dst_icon", "%s/%s.png", button_dir, button_frame_prefix);
                        
                        rename(src_icon, dst_icon);
                    }
                    
                    // Supprimer le dossier temporaire de cette frame
                    char rmdir_cmd[PATH_MAX];
                    snprintf_checked(rmdir_cmd, sizeof(rmdir_cmd), "rmdir_cmd", "rm -rf \"%s\"", frame_temp_dir);
                    (void)system(rmdir_cmd);
                } else {
                    fprintf(stderr, "Frame %d: erreur lors du render (code: %d)\n", 
                            frame_count + 1, render_result);
                }
            } else {
                // Mode normal: exécuter la commande standard
                (void)system(cmd);
            }
            
            // Supprimer le fichier PNG immédiatement
            unlink(frame_png);
            // printf("Frame %d: fichier supprimé\n", frame_count + 1);
            
            // Délai entre frames si spécifié
            if (opts.sleep_delay > 0) {
                struct timespec ts = {0, opts.sleep_delay * 1000000}; // Convertir ms en nanosecondes
                nanosleep(&ts, NULL);
            } else {
                // Petit délai par défaut pour éviter la surcharge
                struct timespec ts = {0, 100000}; // 100µs en nanosecondes
                nanosleep(&ts, NULL);
            }
        } else {
            fprintf(stderr, "Frame %d: erreur lors de la création du PNG\n", frame_count + 1);
        }
        
        free(rgba_data);
        frame_count++;
        
        // Affichage progressif en mode render
        if (opts.render_mode) {
            gettimeofday(&current_time, NULL);
            int elapsed_seconds = current_time.tv_sec - start_time.tv_sec;
            int hours = elapsed_seconds / 3600;
            int minutes = (elapsed_seconds % 3600) / 60;
            int seconds = elapsed_seconds % 60;
            
            printf("\rframe rendered: [%03d/%d] elapsed time: %02d:%02d:%02d", 
                   frame_count, total_frames, hours, minutes, seconds);
            fflush(stdout);
        }
    }
    
    if (opts.render_mode) {
        printf("\n"); // Nouvelle ligne après la progression
    }
    
    printf("Terminé: %d frames traitées\n", frame_count);
    
    if (stop_requested) {
        printf("Arrêt propre après interruption CTRL+C.\n");
    }
    
    // En mode render, copier le dossier final
    if (opts.render_mode && !stop_requested) {
        printf("Copie du dossier final...\n");
        
        printf("video_dir: %s\n", video_dir);
        printf("final_video_dir: %s\n", final_video_dir);
        
        // Créer la commande de copie récursive silencieuse avec progression sur une ligne
        char *cp_cmd = asprintf_alloc("cp_cmd", "cp -r \"%s\" \"%s\" 2>/dev/null", video_dir, final_video_dir);
        
        printf("Copie des fichiers...");
        fflush(stdout);
        
        int cp_result = system(cp_cmd);
        free(cp_cmd);
        
        printf(" Terminé!                \n"); // Espaces pour effacer la ligne
        if (cp_result == 0) {
            printf("Dossier créé avec succès: %s\n", final_video_dir);
        } else {
            fprintf(stderr, "Erreur lors de la copie du dossier (code: %d)\n", cp_result);
        }
    }
    
    // Nettoyer
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    
    // Supprimer le répertoire temporaire
    char *cleanup_cmd = asprintf_alloc("cleanup_cmd", "rm -rf \"%s\"", tmpdir);
    (void)system(cleanup_cmd);
    free(cleanup_cmd);
    
    return 0;
}
