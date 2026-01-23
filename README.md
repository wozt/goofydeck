# GoofyDeck

## Ulanzi_d200_demon (Ulanzi D200)

Petit démon C qui garde le handle HID de la D200 et expose des commandes via un socket Unix `/tmp/ulanzi_device.sock`.

## Build
```
make
```
(dépendances : libhidapi-libusb, zlib)

## Lancer
```
./ulanzi_d200_demon
```
Logs sur stderr ; `ULANZI_DEBUG=1` pour plus de verbosité (manifest, événements boutons, patch ZIP…).

## Commandes (via `nc -U /tmp/ulanzi_device.sock`)
- `ping` : met à jour la petite fenêtre (ping keep-alive).
- `set-brightness <0-100>`
- `set-small-window <mode cpu mem time gpu>`
- `set-label-style <path.json>` : JSON de style global des labels.
- `set-buttons <path.zip>` : envoie un ZIP déjà construit.
- `set-buttons-explicit --button-N=img --label-N=txt` : construit un ZIP en RAM (manifest + icônes, labels optionnels) et envoie en 0x0001.
- `set-buttons-explicit-14 --button-N=img --label-N=txt` : idem mais accepte le bouton 14 (label ignoré pour le 14).
- `set-partial-explicit --button-N=img --label-N=txt` : idem mais en commande partielle 0x000d.
- `set-partial-explicit-14 --button-14=img` : mise à jour partielle du bouton 14 (pas de label).
- `read-buttons` : reste ouvert et stream les événements TAP/HOLD (hold=0.75s, bouton 14 supporté).

## Slots / labels
- Boutons 1-13 (`--button-N`), labels optionnels (`--label-N`), sinon label vide.
- Les labels sont insérés dans le manifest (clé `Text`), icônes sous `icons/<nom>` ; manifest loggé en debug.

## Patch / padding
- Avant envoi, le ZIP est paddé (0..64 octets) pour éviter les octets interdits aux offsets 1016+1024*n. Si encore présent, patch forcé (0x00/0x7c -> 0x01). Log : `sendzip <bytes> (pad=X, patched=Y)` et `[timestamp] patched invalid bytes (...)`.

## Keep-alive
- Le démon envoie un ping automatique toutes les 24s pour garder l’appareil éveillé. Un `ping` manuel réinitialise le compteur.

## Scripts bash associés
- `lib/send_button.sh` : envoie via `set-partial-explicit` (labels supportés).
- `lib/send_page.sh` : envoie via `set-buttons-explicit` (fichiers explicites, labels supportés).
- `lib/send_page_from_folder.sh` : prend les 13 premières images d’un dossier et envoie via `set-buttons-explicit`.
- `lib/flash_icons.sh` : boucle et envoie des icônes aléatoires via `send_page.sh`.
- `lib/send_image_page.sh` : redimensionne/recadre une image en 1280x720, découpe les 14 tuiles et envoie via `set-buttons-explicit-14`.
- `lib/set_brightness.sh`, `lib/ping_alive.sh`, `lib/keep_alive.sh`, `lib/test_button_pressed.sh` : wrappers autour du daemon, sans dépendance Python.

## Installation (dépendances système uniquement)
```
./install.sh
```
Choisit le script d’install selon la distro (apt/pacman/dnf/brew) et installe hidapi/libusb/zlib/imagemagick/librsvg/cairo/ffmpeg + polices Noto. Aucun venv/Python requis.

## Legacy
Les anciens fichiers Python (`ulanzi_d200.py`, etc.) sont conservés sous `legacy/ulanzi_py/` pour référence.
