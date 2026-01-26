# Convertisseur Vidéo Avancé

Ce script permet de convertir des vidéos avec optimisations avancées similaires à `send_image_page.c`.

## Usage

```bash
./convert_video.sh --size=720|480|360 <fichier_video.mp4>
```

## Exemples

```bash
# Convertir en 720p (1280x720)
./convert_video.sh --size=720 video.mp4

# Convertir en 480p (854x480) 
./convert_video.sh --size=480 film.mkv

# Convertir en 360p (640x360)
./convert_video.sh --size=360 clip.avi
```

## Fonctionnalités

### 🎯 **Redimensionnement intelligent**
- **720p** : 1280x720
- **480p** : 854x480  
- **360p** : 640x360

### ✂️ **Crop automatique 16:9**
Le script détecte automatiquement le format d'origine et crop au centre pour obtenir un ratio 16:9 parfait :
- Si la vidéo est plus haute que 16:9 → crop vertical
- Si la vidéo est plus large que 16:9 → crop horizontal
- Si déjà 16:9 → pas de crop nécessaire

### 🎨 **Quantification des couleurs**
- **256 couleurs maximum** par frame
- **Dithering Floyd-Steinberg** pour un rendu optimal
- **Palette optimisée** générée à partir des frames clés

### 🚀 **Optimisations de performance**
- **Threads multiples** pour l'encodage
- **Temporaires dans /dev/shm/** (RAM)
- **Compression H.264** avec CRF 23
- **Faststart** pour streaming web

### 📁 **Gestion des fichiers**
- **Sortie dans le même répertoire** que l'entrée
- **Nom automatique** : `nom_original_converted_720p.mp4`
- **Nettoyage automatique** des fichiers temporaires

## Dépendances

```bash
sudo apt install ffmpeg bc jq
```

- **ffmpeg** : Conversion vidéo
- **bc** : Calculs mathématiques
- **jq** : Parsing JSON (optionnel)

## Exemples de sortie

```
[INFO] Fichier d'entrée: /home/user/video.mp4
[INFO] Fichier de sortie: /home/user/video_converted_720p.mp4
[INFO] Taille cible: 720p
[INFO] Dimensions originales: 1920x1080
[INFO] Durée: 120.5s
[INFO] Crop vertical: 1280x720 (décalage Y: 180)
[INFO] Création de la palette 256 couleurs...
[SUCCESS] Palette créée: /dev/shm/convert_video_abc123/palette.png
[INFO] Conversion vidéo avec dithering Floyd-Steinberg...
frame=  100 fps= 25 time=00:00:04.00 bitrate= 500.0kbits/s
[SUCCESS] Conversion terminée
[SUCCESS] Conversion terminée avec succès!
[INFO] Taille d'entrée: 150MB
[INFO] Taille de sortie: 45MB
[INFO] Ratio: 30%
```

## Algorithmes utilisés

### 🎨 **Quantification des couleurs**
- **Palette 256 couleurs** optimisée avec `palettegen`
- **Dithering Floyd-Steinberg** pour transitions douces
- **Mode rectangle** pour meilleure répartition

### ✂️ **Crop intelligent**
```bash
# Calcul du crop 16:9
ideal_height = width * 9 / 16
crop_y = (original_height - ideal_height) / 2
```

### 🗜️ **Compression**
- **H.264** avec preset medium
- **CRF 23** (bon équilibre qualité/taille)
- **YUV420P** pour compatibilité maximale

## Performance

Le script est optimisé pour la vitesse :
- **Encodage multi-threadé** (4 threads)
- **Temporaires en RAM** (/dev/shm)
- **Palette pré-calculée** (10 frames)
- **Pipeline ffmpeg** optimisé

## Notes

- Le script préserve la piste audio originale
- Supporte tous les formats vidéo reconnus par ffmpeg
- Le fichier de sortie est toujours en MP4 pour la compatibilité
- Les métadonnées sont préservées automatiquement
