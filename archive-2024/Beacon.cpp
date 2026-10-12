//BEACON CODE



#include <LoRa.h>              
#include <TinyGPSPlus.h>       

// Pin Definitions
#define SOS_BUTTON_PIN 13
#define GPS_TX_PIN 17
#define GPS_RX_PIN 16

// LoRa frequency 
#define LORA_FREQUENCY 915E6

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);   // Serial1 for GPS

const char* hikerName = "JohnDoe";
bool sosTriggered = false;

// declaring functions
void checkSOSButton();
void updateGPS();
void broadcastSOS();
void listenForRepeaterSignals();

void setup() {
  Serial.begin(9600);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);

  // Initialize LoRa
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LoRa init failed. Check connections.");
    while (true);
  }
  Serial.println("LoRa Initialized");
}

void loop() {
  checkSOSButton();

  if (sosTriggered) {
    updateGPS();
    broadcastSOS();
  } else {
    listenForRepeaterSignals();
  }
}

void checkSOSButton() {
  if (digitalRead(SOS_BUTTON_PIN) == LOW) {
    sosTriggered = true;
    Serial.println("SOS button pressed!");
  }
}

void updateGPS() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
}

void broadcastSOS() {
  if (gps.location.isValid()) {
    String message = "SOS " + String(gps.location.lat(), 6) + "," + 
                     String(gps.location.lng(), 6) + " " + hikerName;
    Serial.println("Broadcasting: " + message);

    // Broadcast the SOS message
    LoRa.beginPacket();
    LoRa.print(message);
    LoRa.endPacket();
  } else {
    Serial.println("GPS location invalid, cannot broadcast SOS.");
  }
}

void listenForRepeaterSignals() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String receivedMessage;
    while (LoRa.available()) {
      receivedMessage += (char)LoRa.read();
    }

    Serial.println("Received signal to repeat: " + receivedMessage);

    // Rebroadcast the message as a repeater
    LoRa.beginPacket();
    LoRa.print(receivedMessage);
    LoRa.endPacket();
    Serial.println("Message rebroadcasted.");
  }
}