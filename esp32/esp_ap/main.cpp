#include <WiFi.h>

/* ===== Configuration AP ===== */
const char* ssid = "ESP32_RFID_AP";
const char* password = "12345678";   // minimum 8 caractères

IPAddress local_IP(192, 168, 4, 1);   // IP de l’ESP32 AP
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

void setup() {
  Serial.begin(115200);

  // Configure ESP32 as Access Point only
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssid, password);

  Serial.println("ESP32 Access Point started");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  // Nothing to do here
}
