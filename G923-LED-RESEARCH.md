# G923 Xbox RPM LED Research - FFBArcadePlugin

## Objectif
Faire fonctionner les 5 LEDs RPM du Logitech G923 Xbox/PC (PID 0xC26E) avec FFBArcadePlugin, **avec G Hub qui tourne**.

## Branche : `feature/g923-leds` (basee sur master)

## Fichiers modifies
- `Common Files/LogitechLED.h` - Classe LED controller
- `Common Files/LogitechLED.cpp` - Implementation (actuellement v19)
- `DllMain.cpp` - Ajout `g_bypassDIWrapper` pour bypass du wrapper DirectInput

## Architecture du plugin
- `dinput8.dll` wrapper qui hook DirectInput pour les jeux arcade (TeknoParrot)
- Se charge via DLL_PROCESS_ATTACH, LED init dans `LogitechLED::Init()`
- `SetLEDsFromPercent()` appele depuis la boucle FFB du jeu
- Build : macOS dev -> GitHub Actions -> artifacts x86/x64

## Materiel
- Logitech G923 Xbox/PC, VID=0x046D, PID=0xC26E
- G Hub DOIT rester lance (sinon : pas de FFB, pas de boutons)
- HID collections :
  - col02: UP=0xFF43, OUT=20, IN=20 (report 0x11) - **Legacy LED ici**
  - col03: UP=0xFF43, OUT=64, IN=64 (report 0x12) - HID++ 2.0
  - mi_01: UP=0xFFFD, OUT=64, IN=64
  - gamepad: UP=0x0001, IN=11, OUT=0

## Drivers kernel G Hub (la cause du blocage)
- `logi_joy_xlcore.sys` - Translation core
- `logi_joy_bus_enum.sys` - Bus enumerator
- `logi_joy_vir_hid.sys` - Virtual HID
- **Interceptent TOUS les HID output reports** : WriteFile retourne OK mais les donnees ne touchent jamais le bus USB

## Approches testees et resultats

### 1. Direct HID - Legacy [F8 12 mask] (v1-v6)
- Format : `[reportId F8 12 ledMask]` sur col02 (20 bytes, report 0x11)
- WriteFile retourne OK, mais **LEDs ne s'allument pas**
- Confirme par USBlyzer : **zero OUT transfers sur le bus USB** meme pendant le FFB
- Les drivers kernel mangent tout

### 2. HID++ 2.0 Feature Discovery (v3-v4)
- Fonctionne sur col03 (64-byte, report 0x12, device index 0xFF)
- 21 features enumerees dont 0x807A (suspect LED)
- Commandes acceptees mais aucun changement LED
- Meme probleme : drivers kernel interceptent

### 3. LED SDK - sdk_legacy_led_x86.dll (v5-v7)
- Emplacement : `C:\Program Files\LGHUB\sdks\sdk_legacy_led_x86.dll`
- 33 exports, version 75.71.76
- LogiLedInit() -> OK (se connecte a G Hub)
- **Pour clavier/souris UNIQUEMENT, pas pour volant**
- devType=0x8 zones 0-1 retournent OK mais rien ne s'allume
- SetLightingForTargetZone(0x8, 0, ...) = clavier/souris, pas volant

### 4. Steering Wheel SDK - LogitechSteeringWheelEnginesWrapper.dll (v5-v9)
- SDK public v8.75.30 (2018), telecharge depuis Logitech
- 50 exports dont LogiPlayLeds, LogiPlayLedsDInput
- LogiSteeringInitializeWithWindow(false, desktop) -> **OK**
- LogiIsConnected(0) -> **NO** (SDK ne connait pas PID 0xC26E, G923 sorti en 2020)
- Le SDK ne connait que jusqu'au G920 (model 27)

### 5. G Hub Internal Steering SDK (v7)
- Cherche `sdk_legacy_steering_wheel_x86.dll` dans `C:\Program Files\LGHUB\`
- **N'EXISTE PAS** sur la machine de test
- Seuls `sdk_legacy_led_x64.dll` et `sdk_legacy_led_x86.dll` dans le dossier sdks

### 6. Bypass DirectInput Wrapper (v8)
- Notre dinput8.dll wrapper empeche le SDK d'enumerer les devices
- Ajout `g_bypassDIWrapper` flag dans DllMain.cpp
- Quand le SDK appelle DirectInput8Create, on retourne l'interface reelle
- **Resultat : LogiIsConnected(0) toujours NO**
- Le probleme n'est PAS le wrapper, c'est le SDK qui ne connait pas le PID

### 7. LogiPlayLedsDInput Fallback (v9 - PAS ENCORE TESTE)
- Charge le vrai dinput8.dll systeme depuis System32
- Cree un device DirectInput reel pour le G923
- Passe le device a LogiPlayLedsDInput() qui bypass la detection interne
- **Build v9 pret, non teste** (user doit reboot)

### 8. USB Sniffing
- **USBlyzer** : fonctionne avec G Hub, mais capture au niveau HID (au-dessus des drivers)
  - Voit uniquement des IN (device -> host), zero OUT
  - Confirme : les drivers kernel interceptent TOUS les writes
  - PnP Remove Device quand on capture sur le device specifique -> capturer sur le Port/USB composite
- **USBPcap + Wireshark** : pas reussi a configurer (Wireshark ne voit pas USBPcap)
- **Conclusion sniffing** : les drivers kernel redirigent le trafic OUT par un chemin interne invisible aux sniffers HID-level

## Decouverte cle : comment les jeux officiels font
- Les jeux (Forza, F1, etc.) utilisent le meme `LogitechSteeringWheelEnginesWrapper.dll`
- MAIS une **version plus recente** fournie par Logitech aux studios (pas la version publique)
- Cette version connait le G923 PID -> LogiIsConnected retourne YES -> LogiPlayLeds fonctionne
- Le SDK communique avec lghub_agent.exe via IPC, G Hub controle les LEDs via ses drivers kernel
- Un user sur le forum Fanaleds confirme que le SteeringWheelSDKDemo fonctionne avec G923

### 9. Reverse-engineering du Wrapper SDK (v10-v11) - PERCEE MAJEURE
- Analyse binaire de `LogitechSteeringWheelEnginesWrapper.dll` (13 KB, thin wrapper)
- **Aucun PID dans le wrapper** - c'est juste un forwarder
- Imports : `RegOpenKeyExW`, `RegQueryValueExW`, `LoadLibraryW`, `GetProcAddress`
- **Cle registre decouverte** (wide string UTF-16 dans le binaire) :
  `SOFTWARE\Classes\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary`
- Le wrapper lit cette cle pour trouver le VRAI DLL engines installe par G Hub

### 10. Decouverte du vrai moteur SDK v9.1.0 installe par G Hub
- **Cle registre x64** : `HKLM\SOFTWARE\Classes\CLSID\{63BD165D-...}\ServerBinary`
  -> `C:\Program Files\Logi\wheel_sdk\9_1_0\logi_steering_wheel_x64.dll` (93 KB)
- **Cle registre x86** (WOW6432Node) :
  -> `C:\Program Files\Logi\wheel_sdk\9_1_0\logi_steering_wheel_x86.dll` (78 KB)
- Date : 10/02/2026 - installe/mis a jour par G Hub
- **C'est le vrai moteur** que le wrapper charge via LoadLibrary
- Version 9.1.0 vs SDK public 8.75.30 = bien plus recent, devrait connaitre le G923
- Script `tools/scan_pids.ps1` cree pour scanner les PIDs dans ce DLL
- Copie du DLL dans `tools/logi_steering_wheel_x86.dll` pour analyse locale

### 11. Scan PIDs du moteur v9.1.0 - CONFIRME
- Resultat du scan sur `logi_steering_wheel_x86.dll` (78,488 bytes) :
  ```
  0xC298 (DFP)       : PRESENT (2x at 0x00EC3E, 0x0126A0)
  0xC299 (G25)       : PRESENT (1x at 0x00EC1A)
  0xC29A (DFGT)      : PRESENT (1x at 0x00EC0E)
  0xC29B (G27)       : PRESENT (1x at 0x00EC02)
  0xC24F (G29)       : PRESENT (2x at 0x00EC7A, 0x010DAE)
  0xC260 (G920)      : ABSENT
  0xC262 (G920 alt)  : PRESENT (1x at 0x00EC86)
  0xC266 (G923 PS)   : PRESENT (1x at 0x00EC92)
  0xC26E (G923 Xbox) : PRESENT (2x at 0x009D9F, 0x00EC9E) <<<
  ```
- **G923 Xbox est PRESENT** -> le moteur v9.1.0 connait le G923
- **G920 standard (0xC260) est ABSENT** -> Logitech a change la table de PIDs
- **Conclusion** : le probleme est le wrapper v8.75.30, pas le moteur

### 12. Chargement direct du moteur v9.1.0 (v12-v12b)
- Bypass du wrapper : charge `logi_steering_wheel_x86.dll` directement via LoadLibrary
- Chemin lu depuis registre : `HKLM\SOFTWARE\WOW6432Node\Classes\CLSID\{63BD165D-...}\ServerBinary`
- Exports resolus via GetProcAddress : LogiSteeringInitialize, LogiIsConnected, LogiPlayLeds, etc.
- v12b : ajout CoInitializeEx, polling etendu (5x 500ms), multi-index (0-3)
- **Resultat : LogiSteeringInitialize OK, mais LogiIsConnected(0..3) -> toujours NO**
- Le moteur v9.1.0 utilise DirectInput en interne pour detecter les volants

### 13. Scan exhaustif du LED SDK (v13)
- Test de TOUS les device types (0x0 a 0xE) avec TOUTES les zones (0-5)
- Confirme : LED SDK ne supporte que clavier/souris/headset
- Aucun device type ne correspond au volant
- **LED SDK = impasse definitive pour les volants**

### 14. Integration WebSocket G Hub (v14-v14b)
- G Hub expose un WebSocket IPC sur `ws://127.0.0.1:9010` (protocole JSON)
- API : `{msgId, verb, path, payload}` (GET/SET/SUBSCRIBE)
- Implementation WinHTTP dans le plugin C++ (WinHttpOpen, WinHttpWebSocketSend/Receive)
- `/devices/list` retourne ~22KB de JSON avec tous les devices G Hub
- G923 identifie : `id: "dev00000004"`, PID 49774 (0xC26E), deviceType `STEERING_WHEEL`, model `g923_xbox`
- v14b : corrige le parsing deviceId (recherche backward depuis STEERING_WHEEL), activation ACTION avant WHEEL, timeouts WinHTTP
- **Bug v14b** : JSON G Hub contient des espaces (`"deviceType": "STEERING_WHEEL"`) mais le code cherchait sans espaces

### 15. Fix parsing JSON + activation WHEEL (v15)
- Corrige la recherche : cherche `STEERING_WHEEL` en texte brut + `"dev0` pour le deviceId
- STEERING_WHEEL trouve a offset 790, deviceId `dev00000004` correctement parse
- **WHEEL activation : toujours INVALID_ARG** ("Invalid integration GUID")
- Tous les endpoints HID++ via WebSocket : NO_SUCH_PATH
- Tous les endpoints LED/RPM : NO_SUCH_PATH

### 16. Chargement du vrai dinput8.dll systeme (v16)
- Decouverte : `GetModuleHandleA("dinput8.dll")` retourne NOTRE wrapper (puisque nous SOMMES dinput8.dll)
- Fix : `GetSystemDirectoryA` + `LoadLibraryA("C:\Windows\system32\dinput8.dll")` pour le vrai DLL systeme
- DLL systeme charge a 0x685B0000 (adresse differente = vrai DLL)
- DirectInput8Create reussit
- **ZERO devices enumeres** : ni GAMECTRL ni ALL ne retournent de devices
- **DECOUVERTE CRUCIALE** : le G923 Xbox est un device **XInput/GIP**, pas DirectInput
- Windows 8+ cache les devices XInput de l'enumeration DirectInput
- Le moteur SDK v9.1.0 utilise DirectInput pour detecter -> ne peut PAS voir le G923 Xbox

### 17. Analyse de la reponse d'enregistrement (v17)
- Buffer de log etendu de 300 a 800 chars pour voir la reponse complete
- Reponse d'enregistrement (465 bytes) : **AUCUN champ integrationGuid**
- `"integrationType": "INVALID_TYPE"` - l'enregistrement seul ne cree pas de GUID
- Le GUID n'est cree qu'a l'activation

### 18. Activation WHEEL - percee partielle (v18)
- Strategie : ACTION d'abord pour obtenir un GUID, puis WHEEL avec ce GUID
- **WHEEL avec integrationGuid dans le payload** : `INVALID_MESSAGE_RECEIVED` (champ non accepte)
- **WHEEL sans GUID sur `ffb_arcade`** : `INVALID_ARG` (identifier deja consomme par ACTION)
- **Register `ffb_wheel_sdk` + activate WHEEL : SUCCESS !** -> instanceGuid + integrationGuid
- **Decouverte** : chaque sdkType a besoin de son PROPRE integrationIdentifier
- Mais tous les endpoints LED/RPM : toujours NO_SUCH_PATH
- G Hub ne fournit aucune API WebSocket pour controler les LEDs du volant

### 19. WebSocket keep-alive pendant le polling SDK (v19)
- Hypothese : l'activation WHEEL est session-based, fermer le WebSocket tue la session
- WebSocket garde ouvert (handles statiques : g_wsSession, g_wsConnect, g_wsHandle)
- Enregistrement simplifie : `ffb_wheel` + WHEEL uniquement
- **WHEEL activation : SUCCESS, WebSocket reste ouvert**
- Steering SDK polling : LogiIsConnected(0..3) -> **toujours NO**
- DirectInput : toujours zero devices
- **Conclusion** : garder le WebSocket actif ne aide pas le SDK a detecter le G923

## Diagnostic final

### Cause racine identifiee
Le G923 Xbox/PC (PID 0xC26E) est un device **XInput/GIP** (Gaming Input Protocol), PAS un device DirectInput. Windows 8+ cache systematiquement les devices XInput de l'enumeration DirectInput (`IDirectInput8::EnumDevices` ne les retourne jamais).

Le moteur SDK Logitech v9.1.0 (`logi_steering_wheel_x86.dll`) utilise DirectInput en interne pour detecter les volants connectes. Meme si le PID du G923 est present dans le binaire, le SDK ne peut jamais voir le device car Windows ne le liste pas en DirectInput.

### Chaine de blocage
```
G923 Xbox = device XInput/GIP
    -> Invisible pour DirectInput (filtre Windows)
        -> SDK v9.1.0 utilise DirectInput pour la detection
            -> LogiIsConnected() retourne toujours NO
                -> LogiPlayLeds() ne peut pas fonctionner
```

### Comment les jeux officiels font (hypothese mise a jour)
Les jeux (Forza, F1) utilisent probablement :
- Soit une version speciale du SDK qui detecte via **Windows.Gaming.Input (WGI)** au lieu de DirectInput
- Soit un IPC direct avec lghub_agent.exe que le SDK public ne supporte pas
- Le SDK v9.1.0 installe par G Hub est peut-etre utilise UNIQUEMENT par les jeux PS (G29, G923 PS qui sont bien DirectInput)

### Toutes les approches epuisees
| # | Approche | Resultat |
|---|----------|----------|
| 1 | Direct HID Legacy [F8 12] | Drivers kernel interceptent tout |
| 2 | HID++ 2.0 Feature Discovery | Drivers kernel interceptent tout |
| 3 | LED SDK (sdk_legacy_led) | Clavier/souris uniquement |
| 4 | Steering SDK v8.75.30 (wrapper) | Ne connait pas le G923 |
| 5 | G Hub Internal Steering SDK | N'existe pas |
| 6 | Bypass DirectInput wrapper | SDK ne voit toujours pas le G923 |
| 7 | LogiPlayLedsDInput | G923 invisible en DirectInput |
| 8 | USB Sniffing | Confirme : drivers interceptent OUT |
| 9 | Reverse wrapper -> moteur v9.1.0 | PID present mais detection via DI |
| 10 | Chargement direct moteur v9.1.0 | IsConnected toujours NO |
| 11 | Scan exhaustif LED SDK | Impasse definitive |
| 12 | WebSocket G Hub (register/activate) | Pas d'API LED pour volant |
| 13 | System dinput8.dll pour enum | Zero devices (XInput) |
| 14 | WebSocket keep-alive + SDK | SDK toujours aveugle |

## Pistes restantes (non testees)

### A. Windows.Gaming.Input (WGI)
- API moderne Microsoft pour les controllers Xbox/GIP
- Inclut `RacingWheel` class avec support LED potentiel
- Necessite C++/WinRT ou COM, compile comme UWP/desktop bridge
- **Piste la plus prometteuse** car c'est la seule API qui "voit" le G923 Xbox

### B. Kill lghub_agent.exe + HID direct
- Tester si les drivers kernel laissent passer les HID writes quand l'agent est arrete
- Risque : perte du FFB si G Hub est necessaire pour ca aussi
- Test rapide : `taskkill /f /im lghub_agent.exe` puis envoyer [F8 12 mask]

### C. Reverse-engineer l'IPC G Hub
- G Hub WebSocket sur ws://localhost:9010 - explore mais pas d'endpoints LED trouves
- Projet GitHub : LGSTrayBattery_GHUB
- Extraire `app.asar` de G Hub pour trouver des endpoints caches

### D. API Monitor sur lghub_agent.exe
- Hook WriteFile/DeviceIoControl pour voir ce que G Hub envoie reellement
- Fonctionne au user-mode, pas de driver
- http://www.rohitab.com/apimonitor

## Statut : EN PAUSE
Recherche suspendue apres 19 iterations. La cause racine (XInput vs DirectInput) est un blocage fondamental au niveau OS/driver qui ne peut pas etre contourne par du code user-mode classique.

## DLLs sur la machine de test
- `C:\Program Files\LGHUB\sdks\sdk_legacy_led_x86.dll` (3.3 MB)
- `C:\Program Files\LGHUB\sdks\sdk_legacy_led_x64.dll` (4.0 MB)
- `LogitechSteeringWheelEnginesWrapper.dll` (dans le dossier du jeu, SDK v8.75.30)
- `C:\Program Files\Logi\wheel_sdk\9_1_0\logi_steering_wheel_x86.dll` (78 KB) - **VRAI moteur G Hub**
- `C:\Program Files\Logi\wheel_sdk\9_1_0\logi_steering_wheel_x64.dll` (93 KB)

## Commits (du plus ancien au plus recent)
```
9617965 feat: add Steering Wheel SDK support (LogiPlayLeds)
2b6a87d fix: defer steering SDK init, try multiple window handles
bcd2890 feat: enumerate all LED SDK exports, scan wider device types
1feacc3 fix: TrySteeringSDK returns false when wheel not detected
4befd8b feat: try G Hub internal steering wheel SDK (sdk_legacy_steering_wheel)
2e3b63c feat: activate LED SDK devType=0x8 at runtime for G923
a72b6ce fix: bypass dinput8 wrapper for Logitech SDK enumeration
540a7dd feat: fallback to LogiPlayLedsDInput when SDK can't detect G923
43eb3e6 fix: use LPVOID for LogiPlayLedsDInput to fix ANSI/Unicode build
4f1caa4 chore: bump version to v9 for tracking
a2535be fix: use proper __stdcall callback for DirectInput EnumDevices
e7d58dc feat: v11 - add COM init, blind PlayLeds, NULL DInput, HMODULE diagnostics
e47d493 chore: add PID scanner PowerShell script for Windows
4e8975f fix: PowerShell 5.1 compatibility for scan script
5b60d0a feat: v12 - load engine DLL directly via registry, bypass old wrapper
1a88a98 feat: v12b - add COM init, extended polling, multi-index detection diagnostics
379940e feat: v13 - exhaustive LED SDK scan (all device types + zones)
d694505 feat: add G Hub WebSocket explorer script for LED discovery
b86fd65 feat: add HTML WebSocket explorer (no PowerShell needed)
b479fe4 feat: v14 - register with G Hub via WebSocket before engine init
9ff3e4c fix: v14b - fix deviceId parsing, activate ACTION before WHEEL, add timeouts
5daf31f feat: v15 - fix JSON space parsing, WHEEL-first activation, HID++ via WebSocket
17efec5 fix: v16 - load real system dinput8.dll instead of our wrapper for DI enumeration
f902248 feat: v17 - extract integrationGuid from registration, pass in WHEEL activation
c00dcb9 feat: v18 - ACTION-first to get GUID, retry WHEEL with GUID, try integrationType=SDK
638f064 feat: v19 - keep WebSocket alive during steering SDK polling, dedicated WHEEL registration
fc36439 fix: move static WS handles before Close() to fix compilation
```
