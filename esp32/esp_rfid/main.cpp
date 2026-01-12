#include <Arduino.h>       // Bibliothèque principale Arduino 
#include <SPI.h>           // Protocole de communication SPI
#include <MFRC522.h>       // Pilote pour le lecteur RC522
#include <WiFi.h>          // Gestion du WiFi 
#include <PubSubClient.h>  // Client pour le protocole MQTT
#include <ArduinoJson.h>   // Manipulation du format JSON
#include <WebServer.h>     // Serveur Web 
#include <ESPmDNS.h>       // Gerer les noms de domaine a partir des adresses IP
#include "mbedtls/aes.h"   // Bibliothèque de chiffrement AES

// WiFi
const char* ssid     = "ESP32_RFID_AP";
const char* password = "12345678";

// Serveur MQTT
const char* mqtt_server = "192.168.4.2";

WiFiClient espClient;
PubSubClient client(espClient);

// variables pour la commande reçue en MQTT
String actionToPerform = "";
int    sectorToAccess  = -1;
String keyToUse        = "";
String dataText        = "";
String dataNumber      = "";

// pour stocker un float sur 4 octets
union FloatUnion {
  float number;
  byte  bytes[4];
};

// clé AES-128 (16 octets) :RFIDCRYP en hexa

const byte AES_KEY[16] = { 
  0x05, 0x02, 0x04, 0x06,
  0x04, 0x09, 0x04, 0x04,
  0x04, 0x03, 0x05, 0x02,
  0x05, 0x09, 0x05, 0x00
};

// chiffrement d'un bloc de 16 octets
void aesEncryptBlock(const byte in[16], byte out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, AES_KEY, 128);              // 128 bits
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

// déchiffrement d'un bloc de 16 octets
void aesDecryptBlock(const byte in[16], byte out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, AES_KEY, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

//fonction pour afficher un bloc en HEX dans le moniteur série
void printBlockHex(const char* label, const byte *block, int len) {
  Serial.print(label);
  for (int i = 0; i < len; i++) {
    if (block[i] < 0x10) Serial.print("0");  // pour avoir 2 chiffres
    Serial.print(block[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}

// RFID
#define RST_PIN  22
#define SS_PIN    5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// infos affichées sur la page web
String lastUID    = "";
String lastText   = "";
String lastNumber = "";
String lastStatus = "";

// serveur HTTP
WebServer server(80);

// page HTML  pour voir les données
const char* htmlPage = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Dashboard RFID</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>
    body{font-family:system-ui,Arial;margin:16px}
    .card{border:1px solid #ddd;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 2px 6px rgba(0,0,0,.06)}
    .k{color:#666}
    h1{margin:0 0 12px}
    code{background:#f6f8fa;padding:2px 6px;border-radius:6px}
    .ok{color:#0a0}
    .err{color:#a00}
  </style>
</head>
<body>
  <h1>DASHBOARD RFID</h1>
  <div class="card"><b class="k">Statut:</b> <span id="status">-</span></div>
  <div class="card"><b class="k">UID:</b> <code id="uid">-</code></div>
  <div class="card"><b class="k">Texte (blocs 0+1):</b> <span id="text">-</span></div>
  <div class="card"><b class="k">Nombre (bloc 2):</b> <span id="number">-</span></div>
  <script>
    async function refresh(){
      try{
        const r = await fetch('/data.json',{cache:'no-store'});
        const j = await r.json();
        const st = document.getElementById('status');
        st.textContent = j.status || '-';
        st.className = (j.status||'').toLowerCase().includes('erreur')?'err':'ok';
        document.getElementById('uid').textContent = j.uid || '-';
        document.getElementById('text').textContent = j.text || '-';
        document.getElementById('number').textContent = (j.number ?? '-');
      }catch(e){
        const st = document.getElementById('status');
        st.textContent = 'Erreur HTTP';
        st.className = 'err';
      }
    }
    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>
)HTML";

// prototypes
void setupWifi();
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void hexStringToByteArray(String hex, byte *byteArray);
void sendCardUID();
void performWriteSectorAction();
void performReadSectorAction();

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    
  }

  setupWifi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  SPI.begin();
  mfrc522.PCD_Init();

  // accès via http://rfid.local
  if (MDNS.begin("rfid")) {
    Serial.println("mDNS actif: http://rfid.local/");
  }

  // route principale -> page HTML
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage);
  });

  // route qui renvoie les dernières données en JSON
  server.on("/data.json", HTTP_GET, []() {
    String json = "{";
    json += "\"uid\":\""    + lastUID    + "\",";
    json += "\"text\":\""   + lastText   + "\",";
    json += "\"number\":\"" + lastNumber + "\",";
    json += "\"status\":\"" + lastStatus + "\"";
    json += "}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("Serveur HTTP prêt sur le port 80");
  Serial.println("Éditeur RFID secteur prêt");
}

void loop() {
  // MQTT
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // HTTP
  server.handleClient();

  // si pas de nouvelle carte, je ne fais rien
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  // une carte vient d’être lue
  sendCardUID();
  lastUID = "";   

  // selon l’action reçue en MQTT
  if (actionToPerform == "read_sector") {
    performReadSectorAction();
  } else if (actionToPerform == "write_sector") {
    performWriteSectorAction();
  }

  // je vide l’action après usage
  actionToPerform = "";

  // fin de la comm avec la carte
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  delay(3000);
}

// lecture d’un secteur (texte + nombre chiffrés)
void performReadSectorAction() {
  Serial.println("\nLecture secteur");

  MFRC522::MIFARE_Key key;
  hexStringToByteArray(keyToUse, key.keyByte);

  int firstBlock = sectorToAccess * 4; 

  MFRC522::StatusCode status = mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    firstBlock,
    &key,
    &(mfrc522.uid)
  );

  if (status != MFRC522::STATUS_OK) {
    client.publish("iot/projet/rfid/status", "Erreur: Authentification");
    lastStatus = "Erreur: Authentification";
    return;
  }

  Serial.println("Authentification OK, lecture...");

  String textData = "";
  byte buffer[18];
  byte size = sizeof(buffer);

  // lecture du texte sur les 2 premiers blocs du secteur (chiffré AES)
  for (int i = 0; i < 2; i++) {
    int blockAddr = firstBlock + i;
    status = mfrc522.MIFARE_Read(blockAddr, buffer, &size);

    if (status == MFRC522::STATUS_OK) {
      byte cipherBlock[16];
      byte decryptedBlock[16];

      memcpy(cipherBlock, buffer, 16);

      // déchiffrement
      aesDecryptBlock(cipherBlock, decryptedBlock);

      
       printBlockHex("Bloc texte déchiffré : ", decryptedBlock, 16);

      // RECONSTRUCTION DU texte à partir du bloc déchiffré
      for (int j = 0; j < 16; j++) {
        if (decryptedBlock[j] >= 32 && decryptedBlock[j] < 127) {
          textData += (char)decryptedBlock[j];
        }
      }
    } else {
      textData += "[Erreur B" + String(blockAddr) + "]";
    }
  }

  client.publish("iot/projet/rfid/response_text", textData.c_str());
  Serial.print("Texte envoyé (blocs 0+1): ");
  Serial.println(textData);
  lastText = textData;

  delay(50);

  // lecture du nombre chiffré sur le 3ème bloc du secteur
  int numberBlockAddr = firstBlock + 2;
  status = mfrc522.MIFARE_Read(numberBlockAddr, buffer, &size);

  if (status == MFRC522::STATUS_OK) {
    byte cipherBlock[16];
    byte decryptedBlock[16];

    memcpy(cipherBlock, buffer, 16);

    // déchiffrement AES
    aesDecryptBlock(cipherBlock, decryptedBlock);

    // debug : afficher le bloc déchiffré
    printBlockHex("Bloc nombre déchiffré : ", decryptedBlock, 16);

    FloatUnion converter;
    memcpy(converter.bytes, decryptedBlock, 4);

    String numStr = String(converter.number);
    client.publish("iot/projet/rfid/response_number", numStr.c_str());
    Serial.print("Nombre (bloc 2) lu (AES): ");
    Serial.println(numStr);

    lastNumber = numStr;
  } else {
    client.publish("iot/projet/rfid/response_number", "0");
    client.publish("iot/projet/rfid/status", "Erreur: Lecture bloc nombre");
  }

  // maj de l’UID pour l’affichage web
  String uid_str = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uid_str.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " "));
    uid_str.concat(String(mfrc522.uid.uidByte[i], HEX));
  }
  uid_str.toUpperCase();
  uid_str.trim();
  lastUID = uid_str;

  lastStatus = "Lecture secteur reussie.";
  client.publish("iot/projet/rfid/status", lastStatus.c_str());
}

// écriture d’un secteur (texte + nombre chiffrés)
void performWriteSectorAction() {
  Serial.println("\nEcriture secteur");

  MFRC522::MIFARE_Key key;
  hexStringToByteArray(keyToUse, key.keyByte);

  int firstBlock = sectorToAccess * 4;

  MFRC522::StatusCode status = mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    firstBlock,
    &key,
    &(mfrc522.uid)
  );

  if (status != MFRC522::STATUS_OK) {
    client.publish("iot/projet/rfid/status", "Erreur: Authentification");
    return;
  }

  Serial.println("Authentification OK, écriture...");

  // écriture du texte sur les 2 premiers blocs (chiffré AES)
  for (int i = 0; i < 2; i++) {
    int blockAddr = firstBlock + i;

    byte plainBlock[16] = {0};
    byte cipherBlock[16];

    String dataChunk = dataText.substring(i * 16, (i + 1) * 16);
    for (int k = 0; k < dataChunk.length() && k < 16; k++) {
      plainBlock[k] = dataChunk.charAt(k);
    }

    // chiffrement AES du bloc texte
    aesEncryptBlock(plainBlock, cipherBlock);

    // debug : afficher le bloc chiffré
    printBlockHex("Bloc texte crypté : ", cipherBlock, 16);

    status = mfrc522.MIFARE_Write(blockAddr, cipherBlock, 16);
    if (status != MFRC522::STATUS_OK) {
      Serial.print("Echec ecriture bloc texte ");
      Serial.println(blockAddr);
      client.publish("iot/projet/rfid/status", "Erreur: Ecriture bloc texte");
      return;
    }
  }

  Serial.println("Texte écrit sur blocs 0+1 (AES)");

  // écriture du float chiffré sur le 3ème bloc (nombre)
  int numberBlockAddr = firstBlock + 2;

  FloatUnion converter;
  converter.number = dataNumber.toFloat();

  byte plainBlock[16] = {0};
  byte cipherBlock[16];

  memcpy(plainBlock, converter.bytes, 4); // on met le float dans les 4 premiers octets

  // chiffrement AES
  aesEncryptBlock(plainBlock, cipherBlock);

  // debug : bloc nombre chiffré
  printBlockHex("Bloc nombre crypté : ", cipherBlock, 16);

  status = mfrc522.MIFARE_Write(numberBlockAddr, cipherBlock, 16);
  if (status == MFRC522::STATUS_OK) {
    Serial.println("Nombre (bloc 2) écrit (AES).");
    client.publish("iot/projet/rfid/status", "Ecriture secteur reussie.");
  } else {
    Serial.print("Echec ecriture bloc nombre ");
    Serial.println(numberBlockAddr);
    client.publish("iot/projet/rfid/status", "Erreur: Ecriture bloc nombre");
  }
}

// envoi de l’UID de la carte via MQTT
void sendCardUID() {
  Serial.println("Carte detectee, envoi UID...");

  String uid_str = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uid_str.concat(String(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " "));
    uid_str.concat(String(mfrc522.uid.uidByte[i], HEX));
  }

  uid_str.toUpperCase();
  uid_str.trim();

  
  client.publish("iot/projet/rfid/iud", uid_str.c_str());
}

// connexion WiFi
void setupWifi() {
  delay(10);
  Serial.println();
  Serial.print("Connexion a ");
  Serial.println(ssid);
  IPAddress local_IP(192, 168, 4, 3); 
  IPAddress gateway(192, 168, 4, 1);  
  IPAddress subnet(255, 255, 255, 0);

  WiFi.config(local_IP, gateway, subnet); 
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connecte");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// reconnexion MQTT si déconnecté
void reconnect() {
  while (!client.connected()) {
    Serial.print("Connexion MQTT...");

    if (client.connect("ESP32_Editeur_RFID")) {
      Serial.println("OK");
      client.subscribe("iot/projet/rfid/commande");
      client.publish("iot/projet/rfid/status", "Systeme pret.");
    } else {
      Serial.print("echec, rc=");
      Serial.print(client.state());
      Serial.println(" -> retry dans 5s");
      delay(5000);
    }
  }
}

// quand un message arrive sur le topic MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    return;
  }

  const char* action = doc["action"];
  if (!action) {
    return;
  }

  actionToPerform = String(action);
  sectorToAccess  = doc["sector"];
  keyToUse        = doc["keyA"].as<String>();

  if (actionToPerform == "write_sector") {
    dataText   = doc["text"].as<String>();
    dataNumber = doc["number"].as<String>();
    client.publish("iot/projet/rfid/status", "ECRITURE: Presentez une carte.");
  } else if (actionToPerform == "read_sector") {
    client.publish("iot/projet/rfid/status", "LECTURE: Presentez une carte.");
  }
}

// convertit une chaîne hexa en tableau de bytes (pour la clé A)
void hexStringToByteArray(String hex, byte *byteArray) {
  for (int i = 0; i < hex.length(); i += 2) {
    String hexPair = hex.substring(i, i + 2);
    byteArray[i / 2] = (byte) strtol(hexPair.c_str(), NULL, 16);
  }
}

