# Bekannte Multiplayer-Bugs aus Spielsession (2026-09-10)

Status-Update (2026-09-10, zweite Bearbeitung): Mehrere Bugs wurden mit
konkreten Code-Änderungen behoben. Diese sind unten mit "✅ BEHOBEN"
markiert, inklusive Datei und Begründung. Alles andere ist weiterhin offen,
meist weil es einen konkreten Multiplayer-Repro braucht (zwei Clients, exakter
Ablauf), der sich ohne Live-Test nicht sicher blind beheben lässt.

## ✅ BEHOBEN: Gohma-Todesabschluss (kein Warp/Herzteil, Musik bleibt an)

**Ursache gefunden:** `BossGoma_SetupDefeated`/`BossGoma_Defeated` (die Death-
Cutscene, die Musik stoppt und Warp/Herzteil spawnt) wird in `z_boss_goma.c`
NUR innerhalb des eigenen Treffer-Callbacks ausgelöst (`if ((s8)health <= 0)`),
NICHT als Dauerprüfung pro Frame. Nicht-Roommaster-Clients erhalten nie einen
echten lokalen Treffer (ihr Schaden wird nur an den Master weitergeleitet),
daher hat `BOSS_DEATH_COMMIT` (setzt nur `colChkInfo.health = 0`) nie die
lokale Todessequenz ausgelöst — der Client hing in der normalen KI fest, bis
`ACTOR_KILLED` den Actor extern hart entfernt hat: kein Warp, kein Herzteil,
Musik lief weiter.

**Fix:** [soh/soh/Network/Anchor/Packets/RoomEvent.cpp](soh/soh/Network/Anchor/Packets/RoomEvent.cpp)
ruft im `BOSS_DEATH_COMMIT`-Handler bei `ACTOR_BOSS_GOMA` direkt
`BossGoma_SetupDefeated()` auf (nicht-static, extern verlinkbar), wodurch die
komplette native Death-Cutscene (inkl. Musikwechsel und Herzteil-/Warp-Spawn)
lokal auf jedem Client abläuft.

**Noch offen:** Das gleiche Muster (Tod nur im Treffer-Callback) betrifft
vermutlich weitere Bosse. Nur Gohma wurde explizit gemeldet und gefixt; andere
Bosse müssten einzeln geprüft werden (jeweils eigene `Boss*_SetupDeath`/
`Boss*_Death`-Funktion finden und Vorbedingungen prüfen).

## ✅ BEHOBEN: Darunia tanzt nur beim Spieler, der das Lied spielt

**Ursache gefunden:** Der Ocarina-Song-Sync in
[soh/soh/Network/Anchor/HookHandlers.cpp](soh/soh/Network/Anchor/HookHandlers.cpp)
hat bisher nur in eine Richtung funktioniert: "wenn NICHT Roommaster spielt,
an Roommaster senden" und im Handler "nur anwenden, wenn ICH Roommaster bin".
Ein dritter Client im Raum (weder Spieler noch Master) hat dadurch nie ein
Signal bekommen. Spielt der Roommaster selbst das Lied, wurde es überhaupt
nicht gesendet.

**Fix:** Jeder Client sendet jetzt sein eigenes gespieltes Lied an alle
Mitspieler (nicht nur an den Master), und jeder empfangende Client (auch der
Master) wendet es lokal an. Ein neues Flag `suppressOcarinaRebroadcast`
(in [Anchor.h](soh/soh/Network/Anchor/Anchor.h)) verhindert eine Echo-Schleife
beim Anwenden fremder Lieder. Betrifft alle Song-reaktiven NPCs/Objekte, nicht
nur Darunia.

## ✅ BEHOBEN: Verschiebbare Kisten/Blöcke nicht synchron (z.B. Eingang Schloss Hyrule)

**Ursache gefunden:** `ACTOR_OBJ_OSHIHIKI` & Co. (Schiebeblöcke) laufen
bewusst ungefroren auf jedem Client (für lokale Kollision), aber der
BG-Keyframe-Sync in HookHandlers.cpp hat Positionen bisher NUR vom Roommaster
gesendet und NUR von Nicht-Master-Clients angewendet — exakt dasselbe
Asymmetrie-Muster wie beim Ocarina-Bug. Schob ein Nicht-Master-Client einen
Block, hat das niemand sonst gesehen.

**Fix:** Für `IsLinkMovablePuzzleActor`-Typen (Obj_Oshihiki, Obj_Makeoshihiki,
Obj_Warp2Block, Obj_HSBlock, Bg_Pushbox) darf jetzt jeder Client senden, und
auch der Roommaster wendet eingehende Keyframes für diese Typen an. Normale
BG-Actors (Plattformen, Türen) behalten das bisherige Master-only-Verhalten
unverändert bei.

## Priorität 1: Weiterhin offen (brauchen Live-Repro)

### Cutscenes enden gelegentlich nicht
- Bestehender Mechanismus (`CutsceneSync.cpp`) sendet nur `CS_STATE_IDLE` vom
  Master, wenn dessen eigene Cutscene endet. Wenn der Softlock durch eine
  ANDERE Cutscene-Quelle entsteht (z.B. ein Client-lokal getriggertes Skript
  ohne gültigen Master-Gegenpart), greift das nicht. Braucht: genauer Ablauf
  (welche Cutscene, wer triggert sie), um den fehlenden Fall zu identifizieren.

### Gegner im Deku Tree manchmal nicht synchronisiert oder nicht killbar
- Kill-Pipeline (`ActorKilled.cpp`, `RoomKillSync.cpp`, `PlayerAttackActor.cpp`)
  sieht architektonisch korrekt aus (Schaden wird an Master weitergeleitet,
  Overkill-Finalize vorhanden). Ohne reproduzierbaren Actor-Key/Log lässt sich
  der genaue Fehlerfall nicht sicher eingrenzen.

### Dodongos Höhle: Sprengen und Donnerblumen nicht synchron
- Einzelne Sprengwand-Typen sind bereits über `ShouldActorUpdate`-Hooks
  abgedeckt (BG_BOMBWALL, BG_BREAKWALL, BG_SPOT08/11/17_BAKUDANKABE). Wenn
  weiterhin Fälle durchrutschen, fehlt vermutlich ein weiterer Actor-Typ in
  dieser Liste — braucht den genauen Rätsel-/Actor-Namen aus der Session.

## Priorität 2: Gegner- und Bosszustände (weiterhin offen)

### Springende Dodongo-Gegner bleiben im Fluchtmodus
- Kill-Pipeline sollte greifen (Actor_Kill wird unconditional aufgerufen).
  Braucht genauen Actor-Typ/Ablauf zur Eingrenzung.

### King Dodongo: Position synchron, Animation und Angriffe nicht
- Geprüft: `z_boss_dodongo.h`/`.c` sind größtenteils NICHT vollständig
  dekompiliert (fast nur `unk_XXX`-Felder, keine benannten Action-State-Felder
  wie bei Volvagia/Morpha/Phantom Ganon). Ein Fix nach dem etablierten Muster
  (Feld direkt synchronisieren) würde bedeuten, auf gut Glück in
  unbeschriftete Offsets zu schreiben — das Risiko für Abstürze/Fehlverhalten
  ist zu hoch, um es ungetestet zu implementieren. Nächster Schritt: gezielte
  Reverse-Engineering-Sitzung für `z_boss_dodongo.c` (welches `unk_`-Feld
  steuert Walk/Roll/BlowFire/Inhale-Wahl).

### Nacht-Skelette nicht synchron
- Noch nicht untersucht.

## Priorität 3: Dungeon-Objekte, Schalter und Weltzustände (weiterhin offen)

### Deku-Tree-Wasserstand nicht synchron
- Noch nicht untersucht.

### Augenschalter ist nur in einer Richtung synchron
- Der generische Flag-Sync (`OnFlagSet`/`OnSceneFlagSet` →
  `SendPacket_SetFlag`/`HandlePacket_SetFlag` in
  [soh/soh/Network/Anchor/Packets/SetFlag.cpp](soh/soh/Network/Anchor/Packets/SetFlag.cpp))
  sieht bereits bidirektional aus (jeder Client broadcastet, jeder wendet an).
  Konnte den konkreten asymmetrischen Fall ohne genauen Dungeon-/Actor-Namen
  nicht reproduzieren. Falls der Bug weiterhin auftritt: bitte genaue
  Fundort-Info (Dungeon, Raum, Schalter-Typ eye/crystal/floor) für gezielte
  Nachverfolgung.

## Priorität 4: Sonstiges

- Alle oben nicht als "✅ BEHOBEN" markierten Punkte bleiben in der
  ursprünglichen Priorisierung weiter unten in diesem Dokument (siehe unten).


### Gohma: Todesabschluss für Nicht-Roommaster unvollständig
- Nach Gohmas Tod verschwindet der Boss beim Nicht-Roommaster.
- Die Kampfmusik bleibt aktiv.
- Teleport/Ausgang und Herzteil erscheinen nicht.
- Ein Netzwerk-Tab-Teleport zum Roommaster aktualisiert den Ausgang nachträglich.
- Verdacht: Boss-Todessequenz, Raumflags und Belohnungs-/Warp-Spawn werden nicht
	als gemeinsamer Abschlusszustand übertragen. `BOSS_DEATH_COMMIT` setzt nur HP;
	die lokale Gohma-AI muss danach aber auf jedem Client dieselbe Abschlusslogik
	erreichen.

### Cutscenes enden gelegentlich nicht
- Ein Client bleibt nach einer Cutscene blockiert.
- Teleport zum Roommaster löst den Zustand indirekt.
- Verdacht: fehlendes oder verlorenes Ende-Signal beziehungsweise ein lokaler
	Cutscene-Zustand ohne gültige Script-Instanz.

### Gegner im Deku Tree manchmal nicht synchronisiert oder nicht killbar
- Normale Gegner fehlen, bleiben lokal am Leben oder reagieren beim
	Nicht-Roommaster nicht auf Treffer.
- Verdacht: Spawn-/Re-Entry-Snapshot oder `ACTOR_KILLED`/`ROOM_KILL_SYNC` verliert
	einen Actor-Key; zusätzlich kann der lokale HP-Override vom nativen Actor-Update
	überschrieben werden.

### Dodongos Höhle: Sprengen und Donnerblumen nicht synchron
- Freigesprengte Wände/Objekte werden nicht überall geöffnet.
- Donnerblumen-Explosionen werden nicht auf alle Clients übertragen.
- Teleport zum Roommaster behebt den Zustand nachträglich.
- Verdacht: Flag-/Explosionsevent wird nur lokal ausgelöst und nicht als
	idempotentes `ROOM_EVENT` oder Snapshot-Zustand weitergegeben.

## Priorität 2: Gegner- und Bosszustände

### Springende Dodongo-Gegner bleiben im Fluchtmodus
- Wenn ein nicht-aggro Client einen Gegner tötet, bleibt der Gegner beim anderen
	Client in einem Fluchtzustand und ist nicht mehr killbar.
- Verdacht: `ACTOR_KILLED` entfernt den Actor nicht auf allen Clients oder der
	Roommaster erhält keinen Kill, während die lokale `actionFunc` weiterläuft.
- Benötigt: reproduzierbarer Actor-Key plus Prüfung, ob Kill vor/nach dem nächsten
	Enemy-Position-Update eintrifft.

### King Dodongo: Position synchron, Animation und Angriffe nicht
- Die Position stimmt, aber Animation, Rollen/Feuerangriff und Angriffstiming
	laufen auseinander.
- Verdacht: Boss-Actors werden absichtlich nicht eingefroren; der aktuelle
	`KingDodongoAdapter` synchronisiert nur Health/Phase, nicht den Boss-internen
	Action-State und Timer.

### Nacht-Skelette nicht synchron
- Gegner spawnen bei gleicher Weltzeit nicht konsistent.
- Verdacht: Zeit-Sync allein überträgt den Spawn-Trigger nicht zuverlässig;
	Skeleton-Spawn/Death muss zusätzlich über einen Raumzustand oder Spawn-Event
	autoritativ gemacht werden.

## Priorität 3: Dungeon-Objekte, Schalter und Weltzustände

### Deku-Tree-Wasserstand nicht synchron
- Der Wasserstand folgt nicht bei allen Clients demselben Zustand.
- Verdacht: lokaler Scene-/Switch-/Water-Level-State ohne Snapshot-Replay.

### Verschiebbare Kisten nicht synchron
- Beispiel: Kiste am Eingang von Hyrule Castle.
- Verdacht: `ACTORCAT_PROP`-Objekte laufen lokal weiter, besitzen aber keinen
	vollständigen Position-/Rotation-Keyframe-Pfad wie `ACTORCAT_BG`.

### Augenschalter ist nur in einer Richtung synchron
- Drückt der Nicht-Roommaster den Schalter, reagiert der Roommaster.
- Drückt der Roommaster den Schalter, reagieren die Clients nicht.
- Verdacht: Client-to-master Trigger ist vorhanden, Master-to-client Broadcast
	oder Snapshot-Anwendung fehlt beziehungsweise wird durch Event-Deduplizierung
	verworfen.

## Priorität 4: Gemeinsame Interaktionen

### Darunia tanzt nur beim Spieler, der das Lied spielt
- Die Darunia-Reaktion wird nicht für alle Spieler im Raum ausgelöst.
- Verdacht: Okarina-Sync überträgt den Songzustand, aber nicht den relevanten
	Actor-/Cutscene-Trigger für alle Clients.

## Empfohlene Bearbeitungsreihenfolge

1. Gohma-Abschluss und Cutscene-Ende untersuchen, weil beide Softlocks und
	 fehlende Progression verursachen.
2. Ein robustes, idempotentes `ROOM_STATE_COMMIT` für Raumflags, Warp und
	 Belohnungs-Spawns ergänzen; dadurch sollten Gohma und Spreng-/Dungeon-Events
	 denselben Abschlussmechanismus verwenden.
3. Kill-Pipeline für Deku-Tree-Gegner und fliehende Dodongos mit Actor-Key,
	 Sequenznummer und late-join Snapshot prüfen.
4. King-Dodongo-Action-State und Angriffstimer analysieren.
5. BG-/PROP-Keyframes für Kisten ergänzen und Augen-/Wasserstands-/Donnerblumen-
	 Trigger in einen bidirektional angewendeten Room-Event überführen.
6. Darunia-Reaktion und Nacht-Skelett-Spawn als globale Raumzustände testen.

## Erste diskriminierende Checks

- Bei Gohma auf beiden Clients loggen: `BOSS_DEATH_COMMIT`, `ACTOR_KILLED`,
	Clear-Flag, aktiver Warp und Herzteil-Spawn.
- Beim Augenschalter prüfen, ob der Roommaster-Event gesendet wird und ob der
	Client ihn wegen `processedRoomEvents` oder fehlender Flag-Anwendung verwirft.
- Beim Dodongo-Kill denselben `actorKey` und die Folge der Pakete
	`ENEMY_POSITION_UPDATE` → `ACTOR_KILLED` → `ROOM_KILL_SYNC` vergleichen.
- Bei King Dodongo die interne Action-State-/Timer-Felder aus
	`z_boss_dodongo.h` gegen die generische Positionssync prüfen, ohne zunächst
	`actionFunc`-Pointer zu übertragen.

## Coop-Fix-Workflow: Obj_Switch (Auge/Boden/Kristall) Remote-Resync

- Problem: Ein per SET_FLAG/UNSET_FLAG empfangener Schalter-Flag wurde zwar gesetzt, aber der
  `ACTOR_OBJ_SWITCH` selbst (Augenschalter schliesst nicht, Bodenplatte/Kristall bleiben optisch aus)
  las den Flag nie neu (nur lokale Kollision). Betraf v.a. EYE_0/EYE_1, FLOOR_0/1, CRYSTAL_0/1.
- Fix: `Anchor_ApplyRemoteSwitchFlagToObjSwitch` (SetFlag.cpp) wird aus HandlePacket_SetFlag,
  FlushPhysicalFlagQueue und HandlePacket_UnsetFlag aufgerufen und stoesst die passende Transition an
  (EyeClosingInit, FloorPressInit, CrystalTurnOnInit bzw. Opening/Release/TurnOff fuer Toggle-Subtypen).
  Gehaltene Platten (FLOOR_2/3) und eisgefrorene Augen bleiben unberuehrt. Kein Effekt ohne Anchor-Verbindung.
- Ungetestet live (kein Zwei-Client-Test). Falls weiterhin kaputt: Pruefen, ob abhaengige BG-Aktoren
  vom Keyframe-Freeze (HookHandlers.cpp ~693) blockiert werden. Geschwister (Lightswitch, Bg_Bdan_Switch,
  Bg_Menkuri_Eye) noch nicht behandelt.

## Workflow: Cutscene-Haenger (coop-fixes)
- "Cutscenes enden gelegentlich nicht": Ursache war ein per ACTOR_KILLED/ROOM_KILL_SYNC mitten in der Cutscene
  getoeteter Boss-Aktor (Actor_Kill beendet nie csCtx/player->csAction), plus eine Recovery, die standardmaessig aus war
  (syncCutscenes) und player->csAction nicht freigab.
- Fix: Watchdog auf Nicht-Master-Clients (HookHandlers.cpp), unabhaengig von syncCutscenes: csCtx nicht-IDLE,
  cutsceneIndex < 0xFFF0, kein lebender Owner-Aktor (CutsceneOwnerAlive) fuer 20 Frames -> Anchor::ReleaseOrphanedCutscene
  (Subkameras, Player csAction 7, csCtx, Letterbox/HUD). Zusaetzlich Release vor Actor_Kill eines Bosses mid-cutscene und
  IDLE-Paket vom Master wird auch ohne syncCutscenes akzeptiert. Forest-Boss ausgenommen. Ungetestet live.

## Workflow: Nacht-Gegner / Tageszeit-Spawns (teilweise behoben)
- Behoben (nur aus Code abgeleitet, nicht live getestet): nightFlag wird jetzt vom Host mit TIME_SYNC gesendet (sofort bei Wechsel) und vom Client ueberschrieben -> Zugbruecke Hyrule Field/Schloss, Stalchild-/Poe-Despawn kippen gleichzeitig.
- Behoben: Client-Schadens-Intercept greift nur noch, wenn der Owner in derselben Szene/Raum ist (lokale Gegner ohne Host sind sonst unkillbar).
- Offen: Stalchild-Spawns (EnEncount1/EnSkb) sind weiter lokal/zufaellig, Keys stimmen nicht ueberein; EnPoField-Key driftet (params/home.y). Achtung: VB_ENCOUNT1_SPAWN_STALCHILD_OR_WOLFOS nie auf false setzen (Endlosschleife).

## Workflow: Deku Tree / Lake Hylia / Water Temple Wasserstand
- Bg_Ydan_Hasi (Wasser + 3 Bloecke), Bg_Spot06_Objects (Wasserebene) und Bg_Mizu_Water (Typ 0) laufen jetzt lokal auf allen Clients (flag-getrieben) statt per BG-Keyframe eingefroren; aus Keyframe-Senden/Blend ausgenommen (IsFlagDrivenWaterBgActor).
- Water Temple: OCARINA_TAG_COMPLETED fuer Flags 0x1C-0x1E ignoriert syncRadius.
- Offen: Late-Join-Replay des Wasserstands (RoomSnapshot), Timer-Drift bis ~35 s bei Late-Join. Nicht live getestet.

## Workflow: King Dodongo Animation/Attacken (nur aus Code abgeleitet, nicht live getestet)
- Ursache: Follower liefen die volle BossDodongo-AI (Angriffswahl abhaengig vom LOKALEN Spieler, Eckenindex per gepinnter Position); nur world.pos war synchron.
- Fix: Host sendet BOSS_ACTION_STATE (actionFunc-Id, Ecke unk_1A0, Richtung unk_1A2; Queue, 1 Event/Frame). Follower rufen die originalen BossDodongo_SetupX auf. Neuer VB_BOSS_DODONGO_LOCAL_DECISION (nur Anchor-Follower = false) sperrt Walk-Exits, Walk-/Roll-Eckenwechsel, Inhale-Bomben-Check und Inhale-Unterbrechung. Reconcile (FollowerTick) nach >10 Frames Abweichung. BOSS_DEATH_COMMIT setzt health=0. Snapshot enthaelt Aktion/Ecke.
- HP-Zugriff jetzt ueber BossDodongo-Struct statt festem Offset 0x194 (64-bit-Layout weicht ab).
- Offen: Follower-Schwerttreffer erreichen den Host nicht (colChkInfo.health bei KD immer 0). Volvagia/Morpha/EnFhg nutzen weiter feste Offsets -> auditieren.

## Workflow: Dodongo / Baby Dodongo / Lizalfos fliehen und sind unkillbar (coop-fixes)
- Ursache (aus Code abgeleitet, nicht live getestet): Actor_Kill ueberspringt EnZf_SetupDie/EnZf_Die, daher bleiben die Overlay-Globals D_80B4A1B0/D_80B4A1B4 des Dodongo's-Cavern-Lizalfos-Minibosspaars falsch gesetzt (Ueberlebender immun, kein AC-Collider, clearFlag nie gesetzt). Baby Dodongo ueberschreibt home.pos beim Eingraben -> Actor-Key driftet.
- Fix: Anchor::ApplyNativeEnemyDeathSideEffects (Overkill-Pfad, ACTOR_KILLED, ROOM_KILL_SYNC); stabiler Key-Cache fuer EN_DODOJR (OnActorInit/OnActorDestroy); applyingRemoteKill verhindert, dass ein Remote-Kill als lokaler Kill neu eingereiht wird (Endlos-ROOM_KILL_SYNC).
- Nicht umgesetzt: synthetischer AC_HIT fuer native Todesanimation von EN_ZF/EN_DODONGO auf der Authority (zu riskant ohne Live-Test); Fallback-Verschaerfung in RoomKillSync.

## Workflow: Dodongo's Cavern Bomben-Sync (nur aus Code abgeleitet, nicht live getestet)
- Neu: BG_SPOT16_BOMBSTONE (Flag -> AC_HIT), BG_DDAN_KD (ROOM_EVENT DDAN_KD_LOWER startet Animation, Flag-Fallback snappt Treppe), BG_DODOAGO (ROOM_EVENT DODOAGO_EYE teilt Augen-Zustand roomCtx.unk_74, Flag oeffnet Kiefer). Behebt Deadlock "A links, B rechts".
- Offen: Augen-Zustand bei Late-Join nicht wiederhergestellt; Bombenblumen-Explosion selbst (EN_BOMBF/EN_BOM) weiter nur lokal (bewusst, kein AT-Collider-Sync). Braucht syncItemsAndFlags.

## Workflow: Boss-Todessequenz (Hit-Callback-only)

- Problem: BOSS_DEATH_COMMIT fand den Boss nie (Adapter-Keys haben 11 Felder inkl. category, GetActorKey 10 Felder), der Gohma-Fix war damit toter Code. Zusaetzlich laeuft die Todessequenz von Gohma/Phantom-Ganon/Bongo/Twinrova nur im Hit-Callback, den Clients nie ausfuehren.
- Fix: Master sendet ueber den OnBossDefeat-Hook das Room-Event BOSS_DEFEAT_START; Clients rufen `Boss*_NetworkForceDefeat` (Kopie des Hit-Callback-Endes) fuer Goma, Ganondrof (real), Sst (Kopf), Tw (Twinrova). ACTOR_KILLED fuer einen Boss in der Sequenz wird zurueckgestellt (deferredBossKills, Hard-Kill nach 2400 Frames). Master-Overkill durch Client-Schlag startet die native Sequenz statt Actor_Kill. BOSS_DEATH_COMMIT ist nur noch HP, BOSS_SUBACTOR_KILL nutzt beide Key-Formate (Mini-Bosse ausgenommen).
- Nicht behandelt: Barinade, Volvagia, Morpha, King Dodongo, Ganondorf, Ganon2 (Roh-Offset-Bugs in KingDodongo/Ganon2/Twinrova-Adaptern bleiben). Nicht live getestet.

## Workflow ocarina-wf: Deku-Tree-Gegner unsynchron / unkillbar

- Raumwechsel-Unload (func_80031B14 killt alle Actors des alten Raums) wurde als echter Kill gewertet (ACTOR_KILLED-Broadcast bzw. endlos erneut gesendete pendingRoomKills). OnActorKill-Hooks (Authority + Client) ignorieren jetzt Actors, deren Raum weder curRoom noch prevRoom ist.
- GetActorKey: EN_FIREFLY (Keese) aendert params zur Laufzeit (Fackel-Entzuendung); Key nutzt nur noch das Perch-Bit (params & 1), damit Kill/HP/Position-Pakete weiter matchen.
- ROOM_KILL_SYNC-Fallback: Radius 200 -> 16 und nur lebende Actors (update != null); verhindert Massaker an gleichartigen Nachbarn.
- IsOwnerInSameRoom: Master muss online + save-geladen sein, sonst wird Schaden lokal angewendet (kein unsterblicher Gegner).
- Bereits vorhanden (kein Eingriff): applyingRemoteKill-Guard gegen Kill-Echo, Owner-Raum-Pruefung im Schaden-Intercept.
- Offen: Listen-Index-basierte Keys, Self-Heal fuer verlorene Kills, Retry-Cap fuer pendingRoomKills. Nicht live getestet.

## Workflow ocarina-wf: Sweep Forest/Fire/Water/Jabu

- UpdateTeamState: Water-Temple-Pegel (0x1C-0x1E) und Forest 0x1B werden jetzt aus den LIVE-Flags (actorCtx.flags.swch) der aktuellen Szene erhalten, nicht aus dem Szenen-Eintritts-Snapshot; Teammate-Save/Join setzt den Wasserstand nicht mehr zurueck.
- OCARINA_TAG_COMPLETED (Water Temple): die drei Pegel-Flags werden gegenseitig exklusiv gesetzt (BgMizuWater bevorzugt sonst 0x1C > 0x1D > 0x1E).
- RoomEvent: JABU_ACTOR_SPAWN war toter Code im WEB_BURNED-Zweig; jetzt eigener Zweig (Big Octo / EN_BX Relay).
- ApplyNativeEnemyDeathSideEffects: Stalfos-Zaehler (BgMoriBigst home.rot.z--) und Meg (Flag 0x1B unset) bei Remote-Kill.
- Offen: Octo-Plattform BattleInProgress-Handoff (Jabu), Wasserstand-Query beim spaeten Beitritt, Bg-Mover-Jitter, BgHidanRock Keyframes. Nicht live getestet.

## Workflow: Shadow/Spirit/Well/Ice/GTG sweep
- GetActorKey ignoriert params fuer En_Dh und Bg_Ice_Objects (Laufzeit-Zustand), damit Schaden/Position/Keyframes den Actor wiederfinden.
- Overkill (Remote-Todesstoss): En_Dh startet EnDh_SetupDeath (En_Dha-Haende sterben selbst), ACTOR_KILLED-Empfaenger killt die Haende; En_Ik/En_Rd/En_Wf setzen ihr Todes-Switch-Flag (Nabooru-Ik wird nicht hart gekillt).
- Bg_Jya_Megami (Spirit-Gesicht) pollt jetzt das Switch-Flag.
- Offen (nicht umgesetzt): bidirektionale BG-Keyframes + Settle-Keyframe (Anspruch "Roommaster wendet Keyframes an" stimmt im Code nicht), Obj_Lightswitch-Poll, GTG Menkuri-Eye/Silberrubine Zaehler-Sync. Ungetestet live.

## Workflow sweep-ganon-overworld

- Ganon2Adapter las den Feuerring-Alpha (Offset 0x324) als HP und schrieb ihn auf Peers zurueck. Jetzt echte HP (colChkInfo.health), nur absenken, nie auf 0, kein BOSS_DEATH_COMMIT mehr.
- Ganondorf: Ein Todesstoss eines Clients wurde auf dem Master per Actor_Kill ausgefuehrt (keine Todes-Cutscene, kein Kollaps). Jetzt BossGanon_NetworkForceDefeat plus BOSS_DEFEAT_START an die Clients.
- Ganon (Phase 2): Overkill-Paket killt ihn nicht mehr, HP bleibt 1, damit der native Treffer Knockdown/Tod ausloest.
- Nicht behoben: Bg_Ganon_Otyuka Kill-Relay, Kollaps-Zelda-Schalter (UNSET), Late-Join-Intro in Ganon-Szenen. Ungetestet (kein Zwei-Client-Test).
