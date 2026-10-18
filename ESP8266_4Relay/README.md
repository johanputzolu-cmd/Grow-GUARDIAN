# Programmateur hebdomadaire Grow Guardian

## SEED BANK

La carte repliable **SEED BANK**, sous Phase suivie, ouvre un catalogue personnel
a `/seed-bank`. Il contient jusqu'a huit fiches : nom, variete, croissance et
floraison indicatives en jours. Ajouter, modifier et supprimer une fiche se fait
sur cette page ; une suppression demande confirmation. Aucune fiche externe
n'est prechargee et le catalogue ne pilote aucun relais.

Les fiches sont sauvegardees sur l'ESP dans LittleFS (fichiers binaires internes,
API JSON), avec relecture et renommage atomique avant confirmation. Elles
survivent aux redemarrages et aux mises a jour OTA sans effacement du filesystem.
Les limites UTF-8 sont de 64 octets pour le nom et 256 pour la variete.
L'ancien champ notes est reutilise pour la variete, sans perte de son contenu.
Fournisseur, lot et lien ne sont plus affiches ni exportes ; ils sont vides
lors d'une sauvegarde par le nouveau formulaire ou d'un import. Le format
binaire et les champs historiques de l'API sont conserves pour compatibilite.
Les erreurs de stockage sont explicites ; un echec conserve le brouillon.
Comme le tableau existant, ce catalogue n'est pas authentifie : ne pas
l'exposer sur Internet et ne pas y enregistrer de donnees confidentielles.

### Partager et reimporter un CSV

Les fiches proposent aussi **Temps de croissance indicatif (jours)** et
**Temps de floraison indicatif (jours)** : champs facultatifs, entiers de 0 a
9999, renseignes manuellement. Vide signifie non renseigne ; 0 reste une
valeur distincte. Phase suivie utilise ces durees pour les jours restants
theoriques et la date previsionnelle de floraison (germination + croissance).
Le J+ et la date reelle restent independants ; aucun relais n'est commande
par ces estimations. Les anciennes fiches binaires restent lisibles,
avec les deux nouvelles valeurs vides et leur ancien texte comme variete.

Dans SEED BANK, **Exporter le catalogue CSV** telecharge les fiches actuelles ;
**Telecharger le modele vide** fournit les colonnes
`nom;variete;croissance_jours;floraison_jours`.
Ouvrir le fichier dans Excel ou LibreOffice (UTF-8, separateur point-virgule),
ajouter une ligne par fiche, conserver les quatre colonnes puis enregistrer en
CSV UTF-8. Le fichier peut etre partage puis selectionne dans **Importer le
fichier rempli**. L'apercu est sans effet tant que le remplacement n'est pas
confirme. Huit fiches maximum, 16 Ko par fichier ; virgules comme separateur,
champs entre guillemets et notes multilignes sont aussi reconnus.
Les anciens CSV a cinq ou sept colonnes sont acceptes : notes devient variete,
fournisseur/lot/lien sont ignores ; cinq colonnes laissent les durees vides.

L'import remplace le catalogue entier ; exporter une sauvegarde auparavant.
Un CSV vide (en-tete seul) efface toutes les fiches apres confirmation.
Chaque fiche est atomique, mais l'import global ne l'est pas : une coupure
peut laisser un catalogue partiellement importe. Une erreur l'indique
explicitement ; actualiser la liste avant de reessayer. Les dates de suivi
et les relais ne sont pas modifies. L'export protege les cellules pouvant
etre interpretees comme des formules avec une apostrophe, retiree au retour
par l'importeur. Ne pas activer de contenu externe dans un tableur.

## Ecran OLED du tableau de bord

L'OLED 128 x 64 utilise des caracteres taille 2 et n'affiche plus l'adresse IP
(les voyants web Dashboard et Relai ne montrent pas non plus d'IP). Un appui sur FLASH (GPIO0) alterne
entre les valeurs de temperature/humidite de l'air et le pourcentage du sol.
Les valeurs avec leurs unites sont affichees, sans titres, etat ni A0.
Dans **Phase suivie**, saisir la date de germination et, facultativement, celle
du passage en floraison, puis cliquer **Enregistrer les dates**. Elles sont
conservees sur l'ESP dans LittleFS, pas seulement dans le navigateur.
La germination initiale est le 08/10/2026 et la floraison est vide.
Le tableau affiche les deux compteurs J+ ; le compteur de germination apparait
aussi en bas des deux pages OLED. Le jour saisi est J+0 : avec la date initiale,
le 18/10/2026 est J+10. Une date future est affichee comme "Date a venir".
La floraison ne peut pas preceder la germination et peut etre effacee.
Ces dates ne changent pas les programmes des relais. La carte Phase suivie est
repliable, comme SEED BANK ; le selecteur manuel de phase a ete retire.
Le calcul suit les jours calendaires de Paris (ete/hiver),
et reprend automatiquement apres un reset.
Avant synchronisation de l'heure, l'OLED affiche J+-- et le web indique
"Heure indisponible", sans inventer de compteur.
`POST /grow/dates` accepte `germination=AAAA-MM-JJ` et `flowering=AAAA-MM-JJ`
(ou vide), entre 1970 et 2099. `/api` renvoie les dates et les compteurs.
La sauvegarde utilise un fichier temporaire, une relecture et un renommage
atomique ; un echec est signale et conserve les anciennes dates. Un fichier
illisible n'est pas remplace automatiquement par des dates par defaut.
Une lecture DHT22 invalide reste signalee par "Erreur".
Les appuis sont filtres sur 40 ms ; maintenir le bouton ne fait pas defiler.
La page air est selectionnee au redemarrage. Ne pas maintenir FLASH pendant
une mise sous tension ou un reset : GPIO0 sert aussi au mode de programmation.
RESET redemarre le module et ne sert pas a changer de page.

Le relais execute les programmes sans PC ni navigateur ouvert. Il utilise
l'heure de Paris, avec changement automatique ete/hiver par synchronisation NTP.
Apres une coupure, le dernier etat sauvegarde des sorties programmees ou
temporaires est restaure avant la synchronisation de l'heure. Les modes ON et
OFF enregistres reprennent directement. Sans sauvegarde de reprise valide,
les sorties PROG restent arretees jusqu'a reception de l'heure.

## Utilisation

- Chaque fonction (lumiere, ventilation, pompe, option) possede quatre plages.
  Chaque plage est une carte repliable, fermee par defaut : cliquer sur son
  titre pour ouvrir ou masquer ses reglages. Plusieurs cartes peuvent rester
  ouvertes ; les actualisations conservent cet etat.
- Une plage active doit avoir au moins un jour selectionne. Les jours sont ceux
  du **debut** de la plage : dimanche 22:00 a 02:00 couvre lundi jusqu'a 02:00.
- Debut = fin signifie 24 heures a partir du debut, et non une plage vide.
- Le mode continu active la sortie sur toute la plage. Le mode cyclique active
  pendant la duree indiquee, toutes les N minutes, en partant du debut.
- Les plages se cumulent : une sortie reste active tant qu'au moins une plage
  la demande. Le compte a rebours tient compte de cette union.
- Modifier les champs ne suffit pas : cliquer ON, PROG ou OFF enregistre les
  quatre plages et choisit le mode. Les brouillons sont preserves pendant les
  actualisations automatiques.
- La marche temporaire force ON pendant 1 a 1440 minutes sans modifier le mode
  enregistre. A la fin ou a l'annulation, le mode precedent reprend, a l'heure
  courante du programme. Le compte a rebours indique la **fin du temporaire**,
  pas necessairement un arret si le programme demande encore ON.
- Un redemarrage conserve l'heure de fin initiale des temporisations :
  la duree de la panne est donc comptee. Un enregistrement de programme
  annule le temporaire de cette fonction ; un echange de relais annule
  celui des deux fonctions concernees.
- Choisir un relais deja affecte echange les deux affectations.

L'heure reste visible dans l'en-tete du programmateur replie. Elle provient du
relais et avance entre les actualisations. Une perte de connexion est affichee ;
les commandes sont bloquees si le firmware relais n'est pas compatible.

## Conditions sur les mesures

Une carte **Conditions**, repliee par defaut dans le programmateur, contient
une regle globale avec choix du relais physique 1 a 4. La regle est desactivee
a l'installation : l'activer et cliquer **Enregistrer les conditions**.
Cela ne change ni les plages ni les modes ON/PROG/OFF.

La regle peut utiliser la temperature DHT22, l'humidite de l'air DHT22 ou
l'humidite relative du sol YL-69. Un seuil seul est possible ; avec deux seuils,
choisir ET ou OU. Seuls les seuils coches comptent. Le declenchement est
strictement **superieur** au seuil. Pour eviter les oscillations, le retour
normal se fait a seuil moins 0,5 degre ou moins 2 points d'humidite.

Deux actions sont disponibles :

- **Tant que le seuil est depasse** : maintenir la demande conditionnelle
  jusqu'au retour normal.
- **Duree fixe au franchissement** : demander la marche pendant 1 a 1440
  minutes. Un depassement permanent ne relance pas le minuteur. Il faut un
  retour normal puis un nouveau franchissement ; les franchissements pendant
  un minuteur actif ne le prolongent pas. Ce mode exige une heure synchronisee.

La condition ne commande que si la fonction affectee au relais est en **PROG** :
la sortie est active si une plage **OU** la condition demande ON.
OFF ignore la condition et ON force la marche. La marche temporaire manuelle
existante reste independante et prioritaire. La regle suit le numero de relais
physique si les affectations sont echangees.

Le tableau envoie ses mesures au relais environ toutes les cinq secondes,
sans navigateur ouvert. Les valeurs sont traitees en dixiemes. Sans nouvel
echantillon pendant 30 secondes, la demande conditionnelle est annulee ;
un defaut explicite sur un capteur requis l'annule immediatement. Un capteur
non selectionne n'empeche pas le fonctionnement. Un ADC sol a 0 ou 1023 est
considere invalide ; une valeur plausible figee ne permet pas de detecter
tous les debranchements ou defauts. Les plages continuent independamment.
Un minuteur annule par perte de mesures ne repart pas simplement a la
reconnexion : un nouveau franchissement apres retour normal est requis.

Les reglages sont conserves en EEPROM, sans modifier le format des 16 plages.
La fin d'un minuteur conditionnel et l'etat de franchissement sont conserves
dans la sauvegarde de reprise. Apres coupure, ce minuteur ne reprend qu'avec
une heure et des mesures valides, et seulement jusqu'a sa fin initiale.
Sans mesures pendant les 30 premieres secondes, sa reprise est annulee.
Un minuteur deja termine ne se redeclenche pas si les mesures restent hautes.
Le mode maintenu est reevalue sur les nouvelles mesures au redemarrage.
Pour le relais vise par une condition active, une demande conditionnelle
ancienne n'est donc pas restauree aveuglement avant reception des mesures ;
les plages reprennent des que l'heure est connue.

Enregistrer la regle reinitialise son minuteur et son franchissement ; si les
mesures sont deja au-dessus et le mode PROG actif, elle peut demarrer aussitot.
Changer le mode ou l'affectation du relais cible reinitialise aussi son etat.
Les ecritures de reprise ne sont pas effectuees pour chaque echantillon :
seuls les changements d'etat, d'echeance ou de reglage sont sauvegardes.
L'interface distingue les valeurs recues, leur age, une condition inactive,
une perte de mesures et un mode autre que PROG. Quand une condition peut
intervenir, elle n'annonce pas de faux compte a rebours d'arret base sur les
seules plages.

## Reprise apres coupure de courant

Le relais sauvegarde les quatre etats commandes et les heures de fin
temporaires dans LittleFS, avec verification d'integrite et remplacement
atomique du fichier. Les ecritures ont lieu seulement quand ces donnees
changent, pas a chaque passage de la boucle. LittleFS repartit l'usure de la
flash ; les programmes restent dans leur EEPROM existante.

Au redemarrage, les etats sauvegardes sont commandes sans attendre le Wi-Fi
ni Internet. Des que NTP fournit une heure valide, les sorties PROG sont
recalees sur le programme actuel et les temporaires deja termines sont
annules ; ceux encore valides ne reprennent que jusqu'a leur fin initiale.
Un cycle programme reste cale sur ses horaires, il ne recommence pas au debut.

Le controleur physique des relais ne renvoie pas d'accuse de reception.
Une trame envoyee trop tot lors de sa mise sous tension peut etre ignoree.
L'ESP renvoie donc l'etat courant des quatre sorties toutes les cinq secondes,
en plus des changements immediats. Il renvoie des commandes d'etat ON/OFF,
pas des basculements : cela ne redemarre pas les cycles ni les temporisations
et ne provoque pas d'ecriture flash supplementaire.
Cela permet aussi de recaler un controleur relais redemarre separement.
L'API indique toujours un etat commande, pas une confirmation physique :
la validation d'une coupure exige d'observer les relais sans cliquer PROG.

**Attention : une pompe precedemment active peut redemarrer avant que l'heure
ne soit connue, meme si sa plage ou son temporaire a expire pendant la panne.
Sans Internet/NTP, ce dernier etat peut rester actif indefiniment.** C'est la
politique de reprise immediate choisie ; sans horloge secourue par pile, le
module ne peut pas connaitre la duree de la coupure avant synchronisation.
Pendant la coupure, aucun appareil ne peut rester alimente sans alimentation
de secours. Les etats memorises sont les commandes, pas une mesure des contacts.

Un nouveau temporaire exige une heure synchronisee pour enregistrer une
echeance fiable. L'annulation reste disponible sans heure. En cas d'echec
de stockage, le tableau signale que la reprise n'est pas sauvegardee ; les
commandes peuvent etre appliquees mais leur API retourne une erreur explicite.
Ne pas relancer aveuglement un temporaire apres une erreur : consulter l'etat.

Le systeme de fichiers auparavant inutilise est initialise une seule fois.
Apres initialisation, un echec de montage ne provoque jamais de reformatage
automatique. Une sauvegarde corrompue ou appartenant a d'autres programmes est
refusee. Une premiere installation ne peut pas retrouver un etat que l'ancien
firmware n'a jamais enregistre.

## Mise a jour et conservation des reglages

Le firmware v3 migre automatiquement les anciens reglages v2 : l'ancienne plage
devient la premiere plage, active tous les jours ; les trois autres sont
desactivees. Les modes, affectations, horaires et cycles sont conserves.
Ne pas effacer toute la flash lors du televersement. Un retour au firmware v2
ne peut pas relire les reglages v3.

Le module relais necessite un flash USB (pas d'OTA sur cet ESP-01). Le tableau
de bord dispose d'une version OTA et d'une version sans OTA. Pour utiliser
l'interface v3, mettre a jour les deux appareils. Les sources de ce depot
incluent le programmateur v3, les conditions et la reprise apres coupure.

## API v3

`GET /api/status` renvoie `schedulerVersion: 3`, l'heure et quatre programmes.
Chaque programme contient `slots[4]`, `temporaryRemaining` en secondes,
`nextChange` (timestamp Unix ou `null`) et `relayOn`.
Le statut expose aussi `recoverySaved` (sauvegarde de reprise operationnelle),
`restoringOutputs` (dernier etat restaure en attendant l'heure) et, dans chaque
programme, `temporaryAwaitingClock` (echeance temporaire a verifier apres NTP).
`conditionVersion: 1` annonce la prise en charge des conditions ; `condition`
contient les reglages, `saved`, `fresh`, `matched`, `active`, `remaining`
(secondes), `resumePending`, `sampleAge` et les dernieres mesures.
`conditionControlled` dans chaque programme indique qu'une condition peut
modifier ses prochaines transitions.
`nextChange` est la prochaine transition de la sortie en mode PROG, hors
temporaire ; aucun evenement n'est annonce en mode force ou pendant un
temporaire. La recherche couvre huit jours et les transitions heure ete/hiver.

`POST /api/program` utilise un formulaire URL-encode :

- `task`: `light`, `ventilation`, `pump` ou `option` ;
- `mode`: `on`, `prog` ou `off` ; `channel`: 1 a 4 ;
- pour chaque indice 0 a 3 : `s0_enabled` (0/1), `s0_days` (masque 0 a 127,
  lundi bit 0, dimanche bit 6), `s0_type` (`window`/`cycle`), `s0_start` et
  `s0_stop` (`HH:MM`), `s0_duration` et `s0_interval` (1 a 1440 minutes,
  duree <= intervalle). Remplacer `s0_` par `s1_`, `s2_`, `s3_`.

`POST /api/temporary` prend `task` et `minutes` (1 a 1440 ; 0 pour annuler).
Le tableau relaie ces routes sous `/relay/status`, `/relay/program` et
`/relay/temporary`. Les POST ne sont pas repetes automatiquement : en cas de
reponse perdue, consulter l'etat avant de relancer une marche temporaire.

`POST /api/condition` enregistre la regle : `enabled`, `temperatureEnabled`,
`humidityEnabled` (0/1), `channel` (1..4), `humiditySource` (0 air / 1 sol),
`logic` (0 OU / 1 ET), `action` (0 maintenu / 1 minuteur),
`temperatureLimit` (-400..800), `humidityLimit` (0..1000), `duration` (1..1440).
Les seuils sont des **entiers en dixiemes**, contrairement a leur affichage JSON
en degres/pourcent. La route du tableau est `/relay/condition`.

`POST /api/sensors` est appele automatiquement par le tableau. Il transmet
`temperatureValid`, `airValid`, `soilValid` (0/1) et `temperature`, `air`, `soil`
(entiers en dixiemes, memes bornes que les seuils). Les mesures invalides portent
une valeur 0 avec un indicateur de validite a 0 ; elles ne sont jamais utilisees
comme une mesure reelle de zero. La fraicheur est mesuree par `millis()` du
relais a la reception. Le navigateur ne participe pas au transport des mesures.

Ces API HTTP ne comportent pas d'authentification : reserver leur acces a un
reseau de confiance, sans exposition directe sur Internet.

## Tests sans actionner les sorties

Avec Python et un compilateur C++ natif :

```powershell
python .\tests\run_tests.py --compiler "C:\chemin\vers\g++.exe"
```

Le test compile le moteur et extrait directement du sketch les fonctions de
migration, de recherche de transition et de commande des relais. EEPROM,
horloge et UART sont simules : aucun appareil n'est contacte. Les cas couverts
incluent nuits, dimanche/lundi, 24 h, cycles, chevauchements, migration,
changements ete/hiver 2026, reprise ON/OFF/PROG, debordement de `millis()`,
perte complete de RAM, reprise avant NTP, panne plus courte ou plus longue que
le temporaire, echec de sauvegarde atomique, corruption et absence de
reformatage destructeur. Le stockage LittleFS est egalement simule.
Les tests couvrent aussi les seuils stricts, les marges de retour, ET/OU,
air/sol, un seul seuil, les echantillons invalides/perimes, l'union avec les
plages, la priorite des modes, la sauvegarde EEPROM des conditions, la
migration de l'ancienne sauvegarde de reprise et les minuteurs conditionnels
avant/apres coupure.
