// AmbiHue per ESP32-S3: Ambilight Philips -> lampade Hue, con interfaccia web.
//
// Il programma è in ambihue.cpp; la configurazione (Wi-Fi, TV, bridge, lampade) è in config.h
// (copialo da config.example.h la prima volta).
// Impostazioni Arduino IDE (menu Strumenti):
//   Scheda:            ESP32S3 Dev Module
//   Flash Size:        16MB (128Mb)
//   PSRAM:             OPI PSRAM
//   Partition Scheme:  16M Flash (3MB APP/9.9MB FATFS)
//   USB CDC On Boot:   Disabled  (se usi la porta "UART"/"COM")
// Libreria richiesta (Gestione librerie): ArduinoJson di Benoit Blanchon, versione 7.
