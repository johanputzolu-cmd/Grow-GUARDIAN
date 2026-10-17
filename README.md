# Grow Guardian

Tableau de bord ESP8266 OLED et programmateur Wi-Fi autonome a quatre relais.

## Apercu du tableau de bord

Temperature, humidite de l'air, humidite du sol, graphiques et programmateur
repliable des quatre relais.

![Interface Grow Guardian avec mesures et programmateur](images/tableau-de-bord.png)

## Materiel en images

### Capteur d'humidite du sol YL-69

Sonde resistive et module de lecture, utilises pour suivre l'humidite du sol.

![Sonde d'humidite du sol YL-69 avec son module de lecture](images/capteur-sol-yl69.png)

### Module Wi-Fi ESP8266 / ESP-01 a quatre relais

Carte utilisee pour commander la lumiere, la ventilation, la pompe et une sortie optionnelle.

![Carte ESP8266 a quatre relais avec reperes des connecteurs](images/module-esp8266-4-relais.png)

Les annotations de cette capture concernent le firmware d'origine du module.
Avec Grow Guardian, utiliser la configuration et les commandes decrites ci-dessous.
Verifier la tension d'alimentation indiquee sur votre propre carte.

## Firmwares

- `ESP8266_OLED_Hello_modif_OTA/` : tableau de bord, OLED, DHT22, sonde de sol
  YL-69, historiques, arrosages, min/max et mises a jour ArduinoOTA.
  Carte Arduino : NodeMCU 1.0 (ESP-12E Module).
- `ESP8266_4Relay/` : ESP-01 avec controleur serie de quatre relais,
  protocole UART a 115200 bauds. Carte Arduino : Generic ESP8266 Module, flash 1 Mo.
  Ce firmware relais ne dispose pas d'OTA.

## Installation

Installer le support ESP8266 3.1.2 dans Arduino IDE, puis les bibliotheques
Adafruit GFX Library, Adafruit SSD1306 et DHT sensor library avec leurs dependances.
Les bibliotheques Wi-Fi, HTTP, mDNS, LittleFS, EEPROM et ArduinoOTA sont fournies
par le support ESP8266.

Dans chaque dossier de sketch, copier `secrets.example.h` vers `secrets.h`
et renseigner les identifiants du Wi-Fi. Choisir aussi un mot de passe OTA fort
pour le tableau de bord. Ne jamais publier `secrets.h` : ce fichier est ignore
par Git. Ouvrir le fichier `.ino` du dossier correspondant dans Arduino IDE.

Effectuer le premier televersement par USB. Les mises a jour suivantes du
tableau de bord peuvent utiliser son port reseau ArduinoOTA, annonce sous
`grow-dashboard-ota`. Toujours conserver la fonctionnalite OTA dans le firmware
envoye si les mises a jour sans cable doivent rester disponibles.

## Fonctionnement

Le tableau de bord decouvre `grow-relay` par mDNS sur le reseau local et
transmet les commandes aux relais. Lumiere, ventilation, pompe et option
disposent chacune d'une affectation, d'un mode ON/PROG/OFF et d'un programme.
Une nouvelle affectation echange les deux relais si le canal est deja utilise.

En PROG : plage continue ou cycles avec duree et intervalle en minutes
(intervalle de debut a debut). Debut = fin signifie 24 heures.
Les cycles repartent a l'heure de debut chaque jour ; une marche est coupee
a la fin de la plage. Les programmes sont sauvegardes en EEPROM et executes
sur le module relais meme sans tableau de bord.

L'heure du relais est synchronisee par NTP avec le fuseau Europe/Paris
(regles CET/CEST). Sans synchronisation apres demarrage, PROG garde les
sorties eteintes. L'API `/api/status` expose `timestamp` et `localTime`
pour verifier l'heure effective.

Le tableau de bord utilise D4 pour le DHT22, A0 pour le sol et GPIO5/GPIO4
pour SDA/SCL de l'OLED. Adapter les references de calibration du sol au materiel.
Les historiques, arrosages et records du tableau de bord sont stockes dans LittleFS.

## Securite

Le tableau de bord et l'API relais sont en HTTP sans authentification :
ne pas les exposer directement a Internet. Utiliser un reseau de confiance
et un acces distant securise. Ne pas rediriger le port OTA vers Internet.
ArduinoOTA avec mot de passe n'est pas une liaison chiffree.

Tester les sorties avec les LED ou une charge basse tension adaptee.
Le raccordement au secteur exige du materiel approprie et une installation
realisee par une personne qualifiee.
