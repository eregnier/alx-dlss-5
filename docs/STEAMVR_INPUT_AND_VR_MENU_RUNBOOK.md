# 🎮 Runbook Technique : SteamVR Input (`IVRInput`), Menus VR Superposés (`IVROverlay`) & Découplage Manette

Ce document consigne les pièges majeurs, les leçons apprises et les patterns d'architecture éprouvés pour l'interception des entrées VR sous SteamVR, la gestion d'un overlay de configuration en jeu, et la navigation sans conflit avec la boucle de locomotion du jeu hôte.

---

## ⚠️ Les 3 Grands Pièges d'Entrée sous SteamVR

### 1. Le Piège du `GetControllerState` Muet (Legacy vs SteamVR Input)
* **Symptôme** : L'appel à `g_pVRSystem->GetControllerState(i, &state, sizeof(state))` réussit (`true`), mais `state.ulButtonPressed` et `state.ulButtonTouched` restent constamment à `0x0000000000000000`, même lorsque le joueur presse ou touche les boutons physiques (A/B, X/Y, D-Pad, Thumbrest).
* **Cause** : Dès qu'un jeu moderne (comme *Half-Life: Alyx*) utilise l'API moderne **SteamVR Input (`IVRInput`)** avec un manifeste d'actions (`actions.json` / `bindings_*.json`), le runtime SteamVR **mute** les masques de boutons de l'ancienne API legacy `IVRSystem::GetControllerState`.
* **Règle absolue** :
  * Ne jamais se reposer uniquement sur `GetControllerState` pour les boutons lorsque le jeu cible utilise `IVRInput`.
  * Toujours intercepter et lire les actions via `IVRInput::GetDigitalActionData` et `IVRInput::GetAnalogActionData`.

---

### 2. Le Piège des Ensembles d'Actions Inactifs (`ActionSet` Inactif)
* **Symptôme** : Une action existe bien dans le manifeste du jeu (par exemple `/actions/dev/in/ToggleMenu` mappée sur le bouton Y dans `bindings_touch.json`), son handle est résolu sans erreur (`GetActionHandle` renvoie `VRInputError_None`), mais `GetDigitalActionData` renvoie systématiquement `bActive = false` et `bState = false`.
* **Cause** : Dans SteamVR Input, une action **n'est mise à jour par le runtime que si son Action Set parent est explicitement passé à `IVRInput::UpdateActionState`** par la boucle du jeu hôte.
  * *Half-Life: Alyx* active `/actions/move`, `/actions/interact`, `/actions/default`, `/actions/weapon`.
  * Mais le jeu n'active **JAMAIS** `/actions/dev` en partie normale !
* **Solution éprouvée (Injection dans `UpdateActionState`)** :
  1. Hooker le **Slot 4** de la table virtuelle (`vtable`) d'`IVRInput` (`IVRInput::UpdateActionState`).
  2. À chaque frame, lorsque le moteur appelle `UpdateActionState`, intercepter le tableau `VRActiveActionSet_t* pSets`.
  3. Allouer un tableau local et y **adjoindre l'Action Set ciblé** (ex: `/actions/dev`).
  4. Transmettre la liste combinée à l'implémentation originale de SteamVR.
  5. Résultat : SteamVR met à jour l'action à chaque frame sans perturber le moteur du jeu.

```cpp
// Hook VTable du Slot 4 d'IVRInput
static vr::EVRInputError Hooked_UpdateActionState(
    void* thisPtr, 
    vr::VRActiveActionSet_t* pSets, 
    uint32_t unSizeOfVRSelectedActionSet_t, 
    uint32_t unSetCount
) {
    if (g_pVRInput && g_hDevActionSet == vr::k_ulInvalidActionSetHandle) {
        g_pVRInput->GetActionSetHandle("/actions/dev", &g_hDevActionSet);
    }

    constexpr uint32_t MAX_SETS = 32;
    vr::VRActiveActionSet_t combinedSets[MAX_SETS];
    uint32_t count = (std::min)(unSetCount, MAX_SETS - 1);

    for (uint32_t i = 0; i < count; ++i) {
        combinedSets[i] = pSets[i];
    }

    if (g_hDevActionSet != vr::k_ulInvalidActionSetHandle && count < MAX_SETS) {
        combinedSets[count].ulActionSet = g_hDevActionSet;
        combinedSets[count].ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        combinedSets[count].ulSecondaryActionSet = vr::k_ulInvalidActionSetHandle;
        combinedSets[count].unPadding = 0;
        combinedSets[count].nPriority = 0;
        count++;
    }

    return g_originalUpdateActionState(thisPtr, combinedSets, unSizeOfVRSelectedActionSet_t, count);
}
```

---

### 3. Le Piège de la Fausse Combinaison en Marchant (Stick Touch vs DPAD)
* **Symptôme** : En voulant créer un raccourci combo "Pouce sur D-Pad + Stick Haut", le menu s'ouvre intempestivement dès que le joueur pousse le stick pour marcher.
* **Causes identifiées** :
  1. `k_EButton_SteamVR_Touchpad = k_EButton_Axis0 = 32` dans `openvr.h`. Inclure `k_EButton_SteamVR_Touchpad` dans le masque de détection du pouce considérait que toucher le stick était équivalent à toucher le D-Pad !
  2. Le contact capacitif ou le curling de squelette (`skelThumbCurl > 0.20f`) était actif pendant la marche.
  3. L'action de déplacement `/actions/move/in/WalkMantle` était confondue avec un déplacement vers le haut du stick droit.
* **Règle absolue d'exclusion** :
  * Pour détecter le pouce sur les boutons de face (X/Y, A/B, D-Pad), **exclure formellement l'axe du stick lui-même (`Axis0`) et la gâchette (`Axis1`)** :
    ```cpp
    uint64_t excludedMask = (1ULL << vr::k_EButton_Axis0) | (1ULL << vr::k_EButton_Axis1);
    uint64_t faceButtons = (state.ulButtonPressed | state.ulButtonTouched) & ~excludedMask;
    ```
  * En marche normale, `faceButtons` reste à 0, interdisant tout faux déclenchement.

---

## 🎛️ Modèle Universel de Déclenchement & Raccourcis VR

### A. La Combinaison "Double Stick Haut" (Filet de Sécurité Universel)
* En gameplay normal de FPS VR (*Half-Life: Alyx*, etc.) :
  * Le stick gauche contrôle la translation (marche avant/arrière/strafe).
  * Le stick droit contrôle la rotation (snap turn ou smooth turn gauche/droite).
  * **Personne ne pousse les deux sticks vers l'avant simultanément en jeu**.
* **Pattern recommandé** :
  ```cpp
  bool anyStickUp = (rightStickY > 0.40f) || (leftStickY > 0.45f);
  bool bothSticksUp = (leftStickY > 0.65f) && (rightStickY > 0.65f);
  bool isControllerCombo = (leftThumbOnDpad && anyStickUp) || bothSticksUp;
  ```
* Ce pattern garantit :
  1. **Déclenchement direct par le raccourci classique** (Pouce sur bouton de face gauche + Stick vers le haut).
  2. **Déclenchement immédiat de secours** (Pousser les deux sticks vers l'avant à fond).
  3. **Zéro faux positif** lors de la marche normale (le stick droit restant centré).

---

### B. Toggle Rapide Silencieux (Pouce G sur Face Plate + Majeur D Grip >= 70%)
* **Principe épuré & sans ambiguïté** :
  * **Main Gauche** : Pouce simplement **posé sur la face du contrôleur gauche** (zone plate capacitive du joystick, bouton X, bouton Y, ou repose-pouce).
  * **Main Droite** : **Majeur droit enfoncé sur la poignée (Grip)** à au moins **70%**.
  * **Exclusion absolue du stick analogique** : Les axes de translation/marche (`leftStickX`, `leftStickY`) sont rigoureusement exclus pour interdire tout faux positif pendant la marche.
  * **Délai de sécurité / Cooldown (1.2 s)** :
    * Un temps de prise en compte d'au moins **1.2 seconde** est requis entre deux bascules consécutives.
    * Obligation de relâcher les contrôles pour réarmer la bascule, éliminant tout clignotement ou bascule involontaire en boucle.
  * **Action exécutée** : Bascule directe de `g_dlss5UserActive` (DLSS-NR ON $\leftrightarrow$ OFF), aucun menu VR, aucun cercle vert, confirmation audio nette (1800 Hz ON / 650 Hz OFF).

---

### C. Porte de Réarmement (Anti-Rebond & Persistance)
* Ne jamais toggler un menu sur simple front montant sans exiger un **retour complet au repos** de l'ensemble des contrôles impliqués :
  ```cpp
  // Porte de réarmement fermée au déclenchement
  if (toggleCombo && s_toggleGateOpen && (toggleElapsed > 650)) {
      s_toggleGateOpen = false;
      s_lastToggleTime = now;
      ToggleMenu();
      return; // Bloque la navigation sur la frame de toggle
  }

  // Ne se réarme que si tous les contrôles sont revenus à zéro
  if (!leftThumbOnDpad && !anyStickUp && !rightMajeurActive && !bothSticksUp && !isKeyboardCombo) {
      s_toggleGateOpen = true;
  }
  ```

---

## 🕹️ Navigation dans l'Overlay avec le Joystick Gauche

Une fois le menu ouvert, le joueur s'attend à pouvoir naviguer immédiatement avec le joystick de sa main principale.

### 1. Séparation Directionnelle Dominante (Anti-Diagonale)
* Pour éviter qu'en voulant descendre dans la liste on ne modifie par mégarde un curseur de netteté ou de fovéation, séparer les axes par amplitude dominante :
  ```cpp
  if (std::abs(leftStickY) >= std::abs(leftStickX)) {
      if (leftStickY > 0.35f) navUp = true;
      else if (leftStickY < -0.35f) navDown = true;
  } else {
      if (leftStickX < -0.35f) navLeft = true;
      else if (leftStickX > 0.35f) navRight = true;
  }
  ```

### 2. Bornage Strict & Action de Fermeture
* **Navigation bornée** : `NavigateUp()` doit être borné à 0 (ne jamais boucler en arrière sur la dernière ligne), et `NavigateDown()` borné à `MAX_ROWS - 1`.
* **Ligne de fermeture dédiée** : Placer sur la dernière ligne `[ FERMER LE MENU ]` qui appelle `Hide(pOverlay)` sans modifier les paramètres actifs ni désactiver les fonctionnalités du mod.

---

## 🥽 Piège SteamVR Input : Détection Capacitive du Pouce sur Oculus Touch / Quest

### Le Problème
* `FingerCurlThumb` (`/actions/default/in/FingerCurlThumb`) est présent dans le manifeste de base Alyx (`actions.json`) mais **n'est pas mappé** dans `bindings_touch.json`. `GetAnalogActionData` retourne donc systématiquement `0.00`.
* `GetControllerState` (API legacy) retourne un masque de boutons et touches intégralement à `0` sous SteamVR Input.

### La Solution Multi-Sources & Découplage D-Pad vs Joystick
1. **D-Pad Gauche (Capacitif & Squelettique) vs Joystick Gauche (`thumbOnLeftStick`)** :
   * **Constat mesuré (bindings Alyx `game/hlvr/cfg/bindings_touch*.json` + profil `oculus_touch`)** : `/input/joystick` et `/input/thumbrest` (la **zone plate**) exposent bien un `touch`, mais **aucune action d'Alyx ne les lit** (`Walk` = clic du stick, `Teleport` = directions, `FingerCurlThumb` non mappée). Les masques legacy `GetControllerState` sont à 0 (aucune ligne `[BTN-RAW]`), et `legacy_bindings_touch.json` ne mappe pas le thumbrest.
   * **Le curl squelettique n'est pas un capteur de position** : `FromDevice` == `FromAnimation` sur 358/361 échantillons. Il ne rapporte que le **touch capacitif du chapeau de stick** (`0.41`), tout le reste (zone plate *ou* pouce absent) tombe en main ouverte (`0.00-0.08`).
   * **Règle implémentée (`ClassifyLeftThumb` dans [`include/input_combo.hpp`](file:///C:/code/hlx-dlss5/include/input_combo.hpp))** :
     * `onStick` = clic stick (`Walk`) **ou** déflexion (`Teleport`/legacy) **ou** `|stickX|/|stickY| > 0.20` **ou** `thumbCurl >= 0.25` (touch du chapeau).
     * `onFacePlate` = **pas** `onStick` **et** (`bouton de face cliqué` **ou** `thumbCurl > 0.02` **ou** `FingerCurlThumb >= 0.40`).
     * **Priorité absolue au stick** : un pouce sur le joystick n'arme jamais un raccourci.
   * **Limite connue** : le thumbrest n'étant pas exposé, « pouce sur la zone plate » et « pouce absent » sont indistinguables. Pour une détection exacte il faudrait un **action set custom** (`/actions/hlx` + bindings `thumbrest/touch`) via un hook de `SetActionManifestPath` — non fait, trop risqué pour l'input du jeu.
   * **Calibration en jeu** : touche **F9** → 120 frames de `[CAPTURE]` (PLATE/STICK/NONE + Dev/Anim curls) pour revalider les seuils sur un autre contrôleur.
   * **Bug corrigé au passage** : `state.rAxis[2]` (le **grip**) était injecté dans `leftStickX/Y` et `rightStickX/Y` — serrer la poignée gauche était vu comme une déviation du stick. `rAxis[2]` n'est plus lu comme un axe de stick.

2. **Module Centralisé & Tests Automatisés (`InputComboEvaluator`)** :
   * Implémenté dans [`include/input_combo.hpp`](file:///C:/code/hlx-dlss5/include/input_combo.hpp) et couvert par 19 scénarios de tests unitaires automatisés ([`tests/test_input_combos.cpp`](file:///C:/code/hlx-dlss5/tests/test_input_combos.cpp)) exécutés par `ctest` (dont 7 dédiés à la classification pouce stick / zone plate).
   * **Raccourci 1 (Toggle DLSS 5)** : `D-Pad gauche + Majeur droit (Grip / HandCurl >= 70%)`. Aucun menu, bips audio (1800 Hz / 650 Hz). Ne se déclenche JAMAIS si le pouce est sur le joystick ou si le joueur avance.
   * **Raccourci 2 (Toggle Menu Flottant)** : `D-Pad gauche + Index droit (gâchette / TriggerPull >= 70%)`. Fallback secondaire conservé : `D-Pad gauche + Joystick droit vers le haut (> 0.40)`. Affiche ou masque l'overlay sans modifier l'état actif de DLSS 5.
   * **Découplage Majeur / Index** : grip et gâchette sont lus séparément (`HandCurl` vs `TriggerPull`). Grip = toggle DLSS, gâchette = menu. Les deux ensemble = ambigu → aucun déclenchement.
   * **Mesure justifiant le découplage** : dans `hlx_dlss5.log`, un `[USER-QUICK-TOGGLE]` est apparu avec `Grip=0.50` dans le probe (max grip/gâchette) → c'était donc la **gâchette** qui franchissait 0.70, pas le majeur. Le probe `[VRINPUT-PROBE]` journalise désormais `GripD` et `TrigD` séparément pour le vérifier en jeu.
   * **Cooldown & Réarmement** : 1,2s obligatoire avec exigence de relâchement complet des touches avant tout réarmement.

---

## 📋 Checklist Avant Chaque Intégration VR Overlay
- [ ] Le DLL d'injection hooke-t-il `VR_GetGenericInterface` pour récupérer `IVRSystem`, `IVRInput`, `IVROverlay`, `IVRCompositor` ?
- [ ] `IVRInput::UpdateActionState` (Slot 4) est-il intercepté pour injecter les action sets masqués du jeu ?
- [ ] Le rôle des manettes est-il vérifié via `GetTrackedDeviceIndexForControllerRole` et non via la seule énumération d'indices ?
- [ ] Le stick lui-même (`Axis0`) est-il exclu du masque de détection du pouce ?
- [ ] La porte de réarmement empêche-t-elle le menu de clignoter ou de se refermer au relâchement des boutons ?
- [ ] Les axes de navigation sont-ils filtrés par axe dominant pour empêcher les interférences diagonales ?
