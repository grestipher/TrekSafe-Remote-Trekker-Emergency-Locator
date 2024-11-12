//BASE STATION CODE



#include <SPI.h> 
#include <LoRa.h>              
#include <TinyGPSPlus.h>  
    

// Pin Definitions
#define LORA_SS_PIN 5
#define LORA_RST_PIN 14
#define LORA_DIO0_PIN 2

// LoRa frequency 
#define LORA_FREQUENCY 915E6

TinyGPSPlus gps;
String receivedMessage = "";

// Function declarations
void listenForSOS();

void setup() {
  Serial.begin(9600);

  // Initialize LoRa
  LoRa.setPins(LORA_SS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);  // Set LoRa pins

  if (!LoRa.begin(LORA_FREQUENCY)) {  // Initialize LoRa frequency
    Serial.println("LoRa init failed. Check connections.");
    while (true);  // Halt 
  }
  Serial.println("LoRa Initialized - Base Station");

  // Set LoRa to receive mode
  LoRa.receive();
}

void loop() {
  listenForSOS();
}

void listenForSOS() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    receivedMessage = "";
    while (LoRa.available()) {
      receivedMessage += (char)LoRa.read();
    }

    Serial.println("Received SOS message: " + receivedMessage);

    
    // Message format: "SOS <lat>,<lng> <hiker name>"
    int spaceIndex = receivedMessage.indexOf(' ');
    if (spaceIndex != -1) {
      String gpsData = receivedMessage.substring(4, spaceIndex);  // Extract lat,lng
      String hikerName = receivedMessage.substring(spaceIndex + 1);  // Extract hiker name

      float lat, lng;
      int commaIndex = gpsData.indexOf(',');
      if (commaIndex != -1) {
        lat = gpsData.substring(0, commaIndex).toFloat();
        lng = gpsData.substring(commaIndex + 1).toFloat();

        Serial.println("GPS Coordinates: Latitude: " + String(lat, 6) + ", Longitude: " + String(lng, 6));
        Serial.println("Hiker Name: " + hikerName);
        
        // add further logic upto your specs
      } else {
        Serial.println("Invalid GPS data received.");
      }
    } else {
      Serial.println("Invalid message format.");
    }
  }
}