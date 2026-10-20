# ambiHue

Porta i colori dell'Ambilight di una TV Philips sulle lampade Philips Hue, usando un ESP32-S3.
Si comanda da un'interfaccia web (`http://ambihue.local`) e, tramite le scene Hue, anche con Alexa.

## Come si comportano le luci

| Lampada | Quando si collega | Quando la sincronizzazione finisce |
|---|---|---|
| Faretto, Lampada (laterali) | sempre, appena accendi la TV | vanno alla scena scelta |
| Luce tavolo | solo in **modalità cinema**, al massimo al 50% | tornano **esattamente com'erano prima** |
| Luce divano | mai (non è nell'area di intrattenimento) | — |

- **Accensione automatica**: quando la TV si accende, le lampade «sempre» si collegano da sole.
- **Modalità cinema**: si attiva richiamando la scena Hue «Cinema» (per esempio «Alexa, attiva Cinema»)
  oppure dall'interfaccia. Aggiunge le lampade «solo cinema». Se la richiami a TV spenta, vale per
  quando la accendi nei 10 minuti successivi.
- **Spegnere le luci**: dall'interfaccia, con l'interruttore «Luci sincronizzate». Le lampade restano
  libere fino al prossimo spegnimento della TV, poi tutto torna automatico. Se spegni le lampade
  con l'app Hue o con Alexa mentre sono sincronizzate, la sincronizzazione si mette in pausa da sola
  e le lampade restano spente (vedi sotto).

Le lampade, la zona di colore e il comportamento («sempre», «solo cinema», «mai») si cambiano
dall'interfaccia; i valori iniziali sono in `config.h`.

## Installazione sull'ESP32-S3

1. Copia `config.example.h` in `config.h` e compila Wi-Fi (2,4 GHz), IP e credenziali di TV e bridge.
   `config.h` non viene caricato su GitHub.
   Per avere `ENT_USER` ed `ENT_KEY`: `python3 ambihue_web.py pair-ent` (poi premi il pulsante del bridge).
2. Nel bridge Hue serve un'area di intrattenimento con le lampade da sincronizzare (anche la luce tavolo);
   il suo numero va in `ENT_GROUP`. Per vederlo: `python3 ambihue_web.py area`.
3. Nell'app Hue crea una scena chiamata **Cinema** (o scegline un'altra dall'interfaccia):
   Alexa la vede e la attiva con «Alexa, attiva Cinema».
4. Arduino IDE → Gestione schede: **esp32** di Espressif. Gestione librerie: **ArduinoJson** (v7).
5. Menu Strumenti: Scheda *ESP32S3 Dev Module*, Flash *16MB*, PSRAM *OPI PSRAM*,
   Partition Scheme *16M Flash (3MB APP/9.9MB FATFS)*. Poi Carica.
6. Apri il Monitor seriale a 115200 baud: compare l'indirizzo dell'interfaccia.

Con arduino-cli:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB .
arduino-cli upload  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB -p /dev/cu.usbserial-XXXX .
```

## Note

- Mentre il bridge fa streaming, le lampade sincronizzate non obbediscono ai comandi normali.
  L'ESP32 si accorge che le hai spente con app/Alexa in due modi: se il bridge segnala le lampade
  spente, oppure se chiude lo streaming da solo. Se il tuo bridge non fa nessuna delle due cose,
  spegni dall'interfaccia.
- `ambihue_web.py` è la versione per computer (Python) da cui è nato il progetto: serve per
  l'abbinamento (`pair-ent`) e per controllare l'area (`area`). Non ha la modalità cinema.
