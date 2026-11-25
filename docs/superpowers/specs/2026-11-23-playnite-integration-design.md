# Intégration Playnite (extension + LavApollo + clients) — design

*2026-11-23 — validé d'avance par l'utilisateur. Livré dans les prereleases LavApollo 2.4.90 à 2.4.93,
extension LavApollo Link 1.0.1 (`.pext` joint à ces prereleases), et dans LavArtemis Dev seulement.*

## Pourquoi

Dans la refonte, **chaque jeu Playnite est une appli à part** dans LavArtemis, comme Bureau et Playnite
Fullscreen. Aujourd'hui LavApollo ne connaît que les applis saisies à la main dans `apps.json`. Il faut :

1. que la bibliothèque Playnite (jeux installés) apparaisse dans la liste des applis, avec ses jaquettes ;
2. qu'un jeu se lance **dans Playnite** (temps de jeu, scripts, émulateurs, sauvegardes cloud gérés par
   Playnite) et que la session se termine quand le jeu se ferme ;
3. qu'un jeu choisi pendant que **Playnite Fullscreen** tourne s'y lance **sans couper le stream**, et que
   les clients montrent « Playnite — en jeu : X ».

## Objectifs

- Une extension Playnite (« LavApollo Link ») qui exporte la bibliothèque et l'état du jeu en cours, et
  sait fermer le jeu en cours sur demande.
- LavApollo ajoute ces jeux à sa liste d'applis **sans toucher à `apps.json`**.
- Le protocole Moonlight ne change pas : un client qui ne sait rien de Playnite voit des applis normales.

## Non-objectifs

- Pas de modification de la bibliothèque Playnite depuis LavApollo (lecture seule).
- Pas d'automatisation Lossless Scaling (reportée).
- Pas de catégories / filtres dans le web UI de LavApollo pour l'instant.

## Anti-objectifs

- Écrire dans `apps.json` : il reste le fichier de l'utilisateur, éditable à la main et dans le web UI.
- Qu'un problème de l'extension (absente, fichier corrompu, Playnite fermé) casse la liste des applis
  ou un lancement normal.

## Approches considérées

1. **Fichiers partagés dans `%ProgramData%\LavApollo\playnite\`** *(retenue)* — l'extension écrit
   `library.json`, `state.json` et les jaquettes PNG ; LavApollo écrit des demandes dans `commands\`.
   Simple, lisible, sans réseau ni droits particuliers (le service tourne en SYSTEM, Playnite en
   utilisateur : `ProgramData` est lisible/écrivable par les deux). Débogable avec un éditeur.
2. **Tube nommé façon Vibepollo** — plus réactif, mais un serveur et un protocole binaire de plus des
   deux côtés, et rien ne survit à un redémarrage de l'un ou de l'autre.
3. **Synchroniser dans `apps.json`** — les clients et le web UI voient tout sans code, mais on réécrit le
   fichier de l'utilisateur (anti-objectif) et des centaines d'entrées noient ses applis.

Pour lancer un jeu, Playnite fournit déjà l'URI **`playnite://playnite/start/<id>`** (marche en mode
bureau comme en Fullscreen, et démarre Playnite s'il est fermé) : pas besoin d'un lanceur à nous.

## Décisions

### Fichiers échangés (`%ProgramData%\LavApollo\playnite\`)

| Fichier | Écrit par | Contenu |
|---|---|---|
| `library.json` | extension | `{version:1, exportedAt, games:[{id, name, source, platform, favorite, lastActivity, playtime, installed, cover, background}]}` — jeux **non masqués**, installés ou non depuis l'extension 1.1.0 (`installed` absent = installé, ce qu'écrivait la 1.0.x) ; `cover` = nom du PNG dans `covers\` ou vide ; `background` = nom du JPEG dans `backgrounds\` ou vide |
| `covers\<id>.png` | extension | jaquette convertie en PNG 600×900 max (LavApollo n'accepte que le PNG) |
| `backgrounds\<id>.jpg` | extension (1.1.0) | image de fond du jeu, JPEG 1920×1080 max |
| `state.json` | extension | `{version:1, playniteRunning, mode:"desktop"|"fullscreen", pid, game:{id,name,startedAt,pid}|null, updatedAt}` |
| `commands\stop-<n>.json` | LavApollo | `{action:"stop-game", gameId}` ; l'extension le traite puis le supprime |

Écritures atomiques (fichier temporaire puis renommage) des deux côtés.

### Extension Playnite « LavApollo Link »

- Plugin générique (SDK Playnite 6.x, .NET Framework 4.6.2), code dans `LavApollo/tools/playnite/`.
- Export de la bibliothèque : au démarrage, après chaque mise à jour de la bibliothèque, et sur une
  commande de menu « Exporter vers LavApollo ». Jaquettes reconverties seulement si la source a changé.
- `state.json` à chaque démarrage/arrêt de jeu, au démarrage et à la fermeture de Playnite.
- Fermeture d'un jeu demandée : tue l'arbre de processus du jeu (`StartedProcessId` fourni par Playnite).

### LavApollo

- `src/playnite.{h,cpp}` : lecture de `library.json` (+ cache par date de modification), lecture de
  `state.json`, écriture des commandes.
- **Applis virtuelles** : à chaque construction de la liste (`proc::parse`), un jeu Playnite devient une
  appli : `uuid` = id Playnite (GUID stable), `name` = nom, `image-path` = sa jaquette PNG (sans jaquette, `/appasset` répond 404 et les clients
  affichent le nom du jeu, jamais la boîte générique), pas de `cmd`,
  lancement par `detached` = l'URI Playnite. Marquées `playnite-game` (champ interne). Si l'utilisateur a
  déjà une appli du même uuid dans `apps.json`, la sienne gagne.
- Liste rafraîchie quand `library.json` change (vérifié au plus toutes les 5 s, depuis `applist`).
- **Fin de session** : tant qu'une appli Playnite-jeu tourne, LavApollo regarde `state.json` ; quand le
  jeu qu'il a lancé apparaît puis disparaît (ou n'est jamais apparu au bout de 120 s), il termine l'appli.
- **Quitter** une appli Playnite-jeu depuis le client : LavApollo écrit `stop-game`, puis termine l'appli.
- **« Quitter le jeu »** (menu en jeu des clients) : `/playnite-quit-game` écrit `stop-game` pour le jeu en
  cours dans Playnite et garde le stream, qui revient à Playnite (404 si aucun jeu ne tourne).
- **Jeu dans Playnite Fullscreen** : si l'appli en cours est « hôte Playnite » (`playnite-host: true` dans
  `apps.json`, ou, à défaut, une appli dont la commande lance `Playnite.FullscreenApp.exe` ou
  `Playnite.DesktopApp.exe`) et que le client
  lance une appli Playnite-jeu, LavApollo **ne refuse pas** : il ouvre l'URI du jeu, garde l'appli Playnite
  en cours et répond comme une reprise (le stream continue).
- `serverinfo` ajoute `PlayniteGameName` / `PlayniteGameUuid` (jeu en cours d'après `state.json`) et
  `PlayniteHostRunning` (absents si l'extension n'est pas là). `applist` ne liste les jeux qu'avec
  **`playnite=1`** (envoyé par LavArtemis Dev), avec `IsPlayniteGame=1`, et marque l'hôte Playnite
  `IsPlayniteHost=1` : la stable et les autres clients ne voient rien de nouveau.

### Clients (LavArtemis Dev, desktop puis Android)

- Lancer une appli `IsPlayniteGame` pendant que `PlayniteHostRunning` : pas de dialogue « quitter
  l'appli en cours », lancement direct (le serveur répond par une reprise).
- La carte de l'appli en cours affiche « en jeu : <PlayniteGameName> » (texte, jamais l'image du jeu).

## Laissé au constructeur

- Délais exacts (5 s, 120 s), nommage des logs, forme du menu Playnite.

## Ordre des jeux (2.4.93)

Après les applis de l'utilisateur, **le plus récemment joué d'abord** (tri « Activité récente » de
Playnite, `lastActivity` de `library.json`, demandé par l'utilisateur), jamais joués à la fin par nom.
Les clients Dev gardent cet ordre.

## Données pour les écrans du prototype (2026-11-26)

Installé ou non, heures jouées, dernière activité et image de fond de chaque jeu, début de la session et du
jeu en cours, type de PC et liaison : `docs/lavartemis-host-data.md`. Corrige aussi l'ordre : il suivait la
liste d'applis, reconstruite seulement quand rien ne tourne, donc figée tant que Playnite Fullscreen restait
lancé ; `/applist` le prend désormais dans `library.json` tel qu'il est (`playnite::library_snapshot()`).
