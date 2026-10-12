# CLAUDE.md — Blueprint Agent Bridge for Unreal Engine

## Obiettivo

Costruire un sistema che permetta ad agenti AI (Claude Code, Codex) di **leggere e modificare i Blueprint di Unreal Engine senza usare l'interfaccia grafica**: aggiungere, rimuovere e modificare nodi, collegare pin, impostare valori di default, compilare e leggere gli errori.

Il sistema è composto da:
1. **Plugin editor C++ per Unreal** che espone le operazioni sui grafi Blueprint.
2. **MCP server** (un solo server, compatibile con Claude e Codex) che wrappa il plugin.
3. **Skill/istruzioni** per gli agenti su come usare i tool in modo efficace.

## Decisione architetturale fondamentale

**NON modificare i file `.uasset` direttamente (a livello di byte).** Motivi:
- Formato binario con tabelle import/export, GUID per nodi e pin, serializzazione che cambia tra versioni del motore.
- Dopo ogni modifica il Blueprint va ricompilato (classe, skeleton, bytecode): senza compilazione l'asset può risultare corrotto.
- Non è diffabile né mergeabile, quindi manca un ciclo di feedback leggibile.

**Approccio scelto:** passare dal motore stesso in modalità headless, usando le API native dell'editor.

API di riferimento (da verificare sulla versione di UE in uso):
- `UEdGraph`, `UEdGraphNode`, `UEdGraphPin`, `UEdGraphSchema_K2`
- `UK2Node_*` (CallFunction, Event, VariableGet/Set, IfThenElse, ecc.)
- `UEdGraphSchema::TryCreateConnection`, `CanCreateConnection`
- `FBlueprintEditorUtils` (variabili, funzioni, marking as modified)
- `FKismetEditorUtilities::CompileBlueprint`
- `FEdGraphUtilities::ExportNodesToText` / `ImportNodesFromText`
- `FBlueprintActionDatabase` per la ricerca dei nodi disponibili

L'API Python di Unreal espone troppo poco dell'editing dei grafi K2: **la parte seria va in C++**. Python si può usare solo per glue e script di test.

## Modalità di esecuzione

Supportare entrambe, partendo dalla prima:
1. **Commandlet** (`UnrealEditor-Cmd`, senza interfaccia): più semplice, avvio lento. Buono per MVP e CI.
2. **Server locale dentro un editor aperto con `-nullrhi`**: più reattivo, adatto a cicli di iterazione rapidi. Da fare dopo l'MVP.

## Struttura prevista del repo

```
/Plugin/BlueprintAgentBridge/     # plugin UE (C++)
  Source/BlueprintAgentBridge/
    Private/ Public/
  BlueprintAgentBridge.uplugin
/mcp-server/                      # MCP server (TypeScript o Python)
/skills/                          # istruzioni/skill per gli agenti
/test-project/                    # progetto UE minimale per i test
/docs/
CLAUDE.md
```

## API dei tool (bozza)

Ogni tool restituisce JSON strutturato con `ok`, dati e `errors[]`/`warnings[]` chiari.

| Tool | Descrizione |
|---|---|
| `bp_list_assets` | Elenca i Blueprint in un percorso (`/Game/...`) |
| `bp_read_graph` | Restituisce un grafo in formato testuale compatto (vedi sotto) |
| `bp_search_nodes` | Cerca nodi disponibili per testo/classe/categoria, nel contesto di un grafo |
| `bp_add_node` | Aggiunge un nodo (tipo + parametri + posizione opzionale), restituisce l'ID |
| `bp_remove_node` | Rimuove un nodo per ID |
| `bp_connect_pins` / `bp_disconnect_pins` | Collega/scollega pin; errore leggibile se la connessione non è valida |
| `bp_set_pin_default` | Imposta il valore di default di un pin |
| `bp_add_variable` / `bp_add_function` | Variabili e funzioni del Blueprint |
| `bp_compile` | Compila e restituisce errori/warning con riferimento al nodo |
| `bp_auto_layout` | Riposiziona i nodi in modo leggibile |

## Formato testuale del grafo

Parte più importante e più sottovalutata del progetto: l'agente ha bisogno di una rappresentazione **compatta, stabile e round-trippable**, altrimenti lavora alla cieca.

Requisiti:
- ID stabili per nodi e pin (usare i GUID del motore o alias brevi mappati ai GUID).
- Mostrare tipo nodo, classe/funzione, pin con tipo, valore di default e connessioni.
- Molto meno verboso del testo di copia/incolla di UE (che comunque è utile come base e fallback).
- Deve essere possibile leggere → modificare → riscrivere senza perdere informazioni.

Valutare JSON per i tool e un formato tipo pseudo-codice per la lettura umana/agente. Documentare la scelta in `/docs/graph-format.md`.

## Regole di sicurezza sui dati dell'utente

- Lavorare **sempre su una copia o su un branch di controllo versione**; mai modificare asset senza che esista un modo di tornare indietro.
- Prima di ogni scrittura: verificare che l'asset non sia aperto o bloccato in un altro editor (source control checkout, file lock).
- Dopo ogni modifica: compilare e riportare gli errori; non lasciare mai un Blueprint in stato non compilato senza segnalarlo.
- Salvare l'asset solo su richiesta esplicita o dopo compilazione riuscita.
- Operazioni distruttive (rimozione di nodi, cancellazione di variabili usate altrove) devono segnalare cosa verrà impattato.

## Reverse engineering: usa il plugin REA

Se hai bisogno di capire come Unreal funziona internamente per questo progetto (struttura del formato `.uasset`, serializzazione dei grafi, layout di classi come `UK2Node_*`, comportamento di funzioni nel binario dell'editor, ecc.), **usa il plugin REA per fare reverse engineering**.

Indicazioni:
- Prima di analizzare a mano, controlla quali skill e tool espone REA nella sessione corrente e usali.
- Non assumere comandi o nomi di tool di REA: leggi le sue descrizioni e istruzioni.
- Se il codice sorgente di Unreal è disponibile (Epic Games Launcher o repo GitHub di UE), **leggilo per primo**: REA serve per ciò che il sorgente non spiega (formati binari, comportamenti non documentati, funzioni non esposte).
- Documenta ogni scoperta in `/docs/reverse-engineering/` con: cosa hai cercato, come l'hai trovato, versione di UE a cui si riferisce.
- Non copiare in questo repo codice sorgente di Unreal o dump di binari: tieni solo note, strutture e riferimenti ai nomi di simboli.

## Roadmap

**Fase 0 — Ricerca**
- Cercare su GitHub progetti MCP per Unreal già esistenti e valutare cosa si può riusare.
- Fissare la versione di UE di riferimento (es. 5.5) e creare `/test-project`.

**Fase 1 — MVP (commandlet)**
1. `bp_read_graph` su Event Graph di un Blueprint semplice.
2. `bp_add_node` per: chiamata a funzione, variabile get/set, Branch, eventi custom.
3. `bp_connect_pins` con errori chiari.
4. `bp_set_pin_default`, `bp_compile` con errori strutturati.
5. MCP server minimale che chiama il commandlet.
6. Test end-to-end: l'agente costruisce un piccolo gameplay (es. "al BeginPlay, stampa un messaggio dopo 2 secondi").

**Fase 2 — Qualità**
- `bp_search_nodes` basata su `FBlueprintActionDatabase`.
- Auto-layout.
- Server persistente con editor `-nullrhi`.
- Funzioni, variabili, macro.

**Fase 3 — Casi speciali**
- Timeline, widget Blueprint, animation graph, Material graph (potenzialmente fuori scope).

## Convenzioni di lavoro

- Lavora per passi piccoli e verificabili; dopo ogni passo esegui un test reale sul progetto di prova.
- Scrivi test automatici (Unreal Automation Tests) per ogni tool.
- Segui lo stile di codice Unreal (prefissi `U`, `F`, `A`, `E`; `UPROPERTY`/`UFUNCTION` corretti).
- Non fare assunzioni sulle API di UE a memoria: verifica sempre sul sorgente della versione scelta.
- Se una decisione cambia l'architettura, aggiorna questo file.
- Quando sei bloccato su un comportamento del motore, prova nell'ordine: sorgente UE → documentazione → REA.

## Rischi noti

1. Compatibilità tra versioni del motore (le API K2 cambiano spesso).
2. Nomi di funzione, classe proprietaria e tipi dei pin: servono ricerca e messaggi d'errore molto chiari.
3. Layout dei nodi creati da codice (senza criterio il grafo diventa illeggibile per gli umani).
4. Asset aperti o bloccati in altri editor.
5. Nodi con pin dinamici (wildcard, Make Array, Switch, ecc.) che richiedono logica dedicata.
