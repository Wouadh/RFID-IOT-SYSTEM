#include <WiFi.h>


const char* ssid = "ESP32_RFID_AP"; // nom d utilisateur  
const char* password = "12345678";   // mot de passe du wifi

IPAddress local_IP(192, 168, 4, 1);   // IP de l’ESP32 AP
IPAddress gateway(192, 168, 4, 1);    // marquer l esp AP comme gateway pour gerer tout le reseau
IPAddress subnet(255, 255, 255, 0); // masque de reseau (jusqua 254 appareils sur le resau)

void setup() {
  Serial.begin(115200);

  // Configurer l ESP32 comme point d acces
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet); //l'ESP32 prendra lIP par défaut
  WiFi.softAP(ssid, password);
// affichage de l adresse IP
  Serial.println("ESP32 Access Point started");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  
}

