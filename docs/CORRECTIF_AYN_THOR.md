# Correctif AYN Thor — GPU à 0 % / jeu en diaporama

Ce document décrit le diagnostic et les modifications apportées pour corriger le bug
observé sur l'**AYN Thor** (Snapdragon, Adreno) : le GPU n'est pas utilisé (0 % dans le
moniteur KGSL), la console ne chauffe pas, le CPU est loin d'être saturé, la RAM a de la
marge — et pourtant le jeu avance en diaporama.

## Diagnostic

### Ce qui n'est PAS le problème

- **Le boost GPU fonctionne.** « Force maximum clocks » (`force_max_clock`) active
  `TurboMode`, qui sur Android appelle `adrenotools_set_turbo(true)` : les fréquences du
  GPU sont bien montées côté KGSL. Le boost n'est pas en cause.
- **Le backend de rendu est bien Vulkan** (le port Android n'utilise pas le renderer Null)
  et la création du renderer ne tombe pas en erreur (le jeu s'affiche).
- **Les ressources ne sont pas saturées** : c'est précisément l'indice clé. Un GPU à 0 %
  avec un CPU tranquille ne signifie pas « le GPU est cassé », mais « le GPU ne reçoit
  presque aucun travail » : le pipeline est bloqué *en attente*, pas en calcul.

### La cause : le vote de fréquence d'affichage qui s'auto-verrouille

Le port Android de Lichii contient un système propre à ce fork : `EmuWindow_Android`
vote la fréquence de rafraîchissement de l'écran via `ANativeWindow_setFrameRate...`
(`src/android/app/src/main/jni/emu_window/emu_window.cpp`) d'après la vitesse de
présentation *observée* de l'émulateur.

Séquence du bug sur le Thor :

1. Au lancement, la compilation des shaders fait chuter la présentation à 3–10 FPS.
2. Le vote descend à cette valeur (`ANativeWindow_setFrameRate(surface, ~7)`) : le
   panneau AMOLED du Thor accepte des modes bas et **descend réellement à ~7–10 Hz**.
3. La présentation est en mode FIFO (vsync) : chaque frame attend le vblank de l'écran.
   Le rythme de présentation devient donc égal au rythme de l'écran → le taux observé
   reste bas → le vote reste bas. **Boucle fermée : la valeur se verrouille pour
   toujours**, même une fois les shaders compilés et le jeu capable de tourner vite.
4. Résultat : diaporama permanent, GPU quasi idle (0 % KGSL : quelques rafales de
   quelques ms par frame seulement), CPU en attente de vblank, aucune chauffe.

Sur le téléphone de test du projet (Galaxy S26, panneau LTPO 120 Hz) l'effet est
atténué ; sur le Thor il est dramatique.

## Modifications

### 1. Plancher le vote d'affichage à 30 Hz — `emu_window.cpp/.h`

`GetFrameRateHint()` est maintenant un wrapper qui impose un plancher à **30 Hz** au
vote envoyé à l'écran (`GetRawFrameRateHint()` contient l'ancienne logique, inchangée).
L'écran ne peut plus descendre sous 30 Hz à cause de l'émulateur : le verrouillage ne
peut plus se former, et le vote remonte toujours vers 60/120 Hz dès que le jeu tient la
vitesse. Une ligne de log a été ajoutée à chaque changement de vote
(`Surface frame rate hint: 60 -> 7.5 Hz`) pour rendre le mécanisme visible dans
`eden_log.txt`.

### 2. Présentation sur le thread GPU pour Adreno propriétaire — `vk_present_manager.cpp`

`docs/DriverBugs.md` de ce dépôt liste un bug connu du driver propriétaire Qualcomm :
**« Concurrent fence waits are unsupported »**. Avec la présentation asynchrone (thread
de présentation séparé, activé par défaut), le thread GPU attend des fences
(`present_done`) pendant que le thread de présentation acquiert des images et soumet :
le driver sérialise alors tout le device et le GPU reste inactif entre les frames.

Sur le driver **Qualcomm propriétaire**, la présentation se fait désormais sur le thread
GPU (un seul « waiter »). Pour réactiver le thread de présentation malgré tout :
`EDEN_FORCE_ASYNC_PRESENT=1` dans `env.txt`.

### 3. Watchdog de blocage actif sur Android — `vk_master_semaphore.cpp`

`EDEN_NX2_HANG_WATCH` est maintenant **actif par défaut sur Android** (désactivable avec
`EDEN_NX2_HANG_WATCH=0`). Si un tick GPU n'avance plus, l'attente est journalisée toutes
les 3 secondes au lieu de bloquer silencieusement — c'est l'outil de diagnostic d'un
éventuel blocage résiduel côté driver.

## Comment tester

1. Installer l'APK reconstruit depuis ce dépôt (ou `./gradlew assembleMainlineRelWithDebInfo`).
2. Lancer un jeu, laisser la compilation de shaders se terminer.
3. Vérifier dans `Android/data/dev.lichii.android/files/log/eden_log.txt` :
   - `Surface frame rate hint: ...` : la valeur ne doit plus descendre sous 30 ;
   - `Qualcomm Adreno: presenting on the GPU thread...` : confirme la contournement du
     bug driver ;
   - toute ligne `Waited ... s for the GPU thread to submit tick ...` ou
     `Waiting for GPU tick ... failed` signale un blocage résiduel à investiguer.
4. Surveiller le GPU (moniteur KGSL / dashboard AYN) : la charge doit maintenant
   apparaître et monter avec la résolution interne.

## Réglages recommandés sur AYN Thor

- **Driver GPU** : installer un driver **Turnip récent mais pas cassé** via le gestionnaire
  de drivers de l'app (les retours communautaires indiquent que certains builds Turnip
  T29 sont défectueux sur ce modèle ; T28 et les builds corrigés plus récents
  fonctionnent). Le driver propriétaire Qualcomm fonctionne aussi avec ce correctif.
- **Résolution** 1×, **mode portable**, cache de shaders disque activé.
- Si un titre reste bloqué, tester `EDEN_NX2_SPLIT=0` dans `env.txt` (désactive le split
  du thread GPU, fonction non testée sur Snapdragon).

## Fichiers modifiés

| Fichier | Changement |
|---|---|
| `src/android/app/src/main/jni/emu_window/emu_window.cpp` | Plancher 30 Hz sur le vote d'affichage + log du vote |
| `src/android/app/src/main/jni/emu_window/emu_window.h` | Déclaration `GetRawFrameRateHint()` |
| `src/video_core/renderer_vulkan/vk_present_manager.cpp` | Adreno propriétaire : présentation sur le thread GPU (contournable par env) |
| `src/video_core/renderer_vulkan/vk_master_semaphore.cpp` | HangWatch actif par défaut sur Android |
| `docs/CORRECTIF_AYN_THOR.md` | Ce document |
| `README.md` | Section « AYN Thor / Snapdragon » |
