# TrekSafe-Remote-Trekker-Emergency-Locator



TrekSafe is a low-power, long-range emergency beacon designed to aid in locating trekkers in remote areas. Using an ESP32, GPS, and LoRa module, each beacon broadcasts an SOS signal with GPS coordinates and a unique hiker ID when activated. Beacons can act as repeaters, boosting other signals to ensure they reach base stations. The system intelligently adjusts broadcast frequency based on battery life, providing efficient emergency communication and enhanced safety for off-grid adventures.




To build your own implementation follow this wiring schematic




BASE STATION 

[LoRa Module]                     [ESP32]
   |                                   |
   |-- SS (Chip Select) ----> GPIO 5   |
   |-- Reset Pin --------------> GPIO 14|
   |-- DIO0 Pin ---------------> GPIO 2 |
   |-- VCC -------------------> 3.3V/5V |
   |-- GND -------------------> GND    |

[GPS Module]                       [ESP32]
   |                                    |
   |-- TX (Transmit) --------------> GPIO 17|
   |-- RX (Receive) --------------> GPIO 16|
   |-- VCC -------------------> 3.3V/5V  |
   |-- GND -------------------> GND     |




BEACON

[LoRa Module]                     [ESP32]
   |                                   |
   |-- SS (Chip Select) ----> GPIO 5   |
   |-- Reset Pin --------------> GPIO 14|
   |-- DIO0 Pin ---------------> GPIO 2 |
   |-- VCC -------------------> 3.3V/5V |
   |-- GND -------------------> GND    |

[GPS Module]                       [ESP32]
   |                                    |
   |-- TX (Transmit) --------------> GPIO 17|
   |-- RX (Receive) --------------> GPIO 16|
   |-- VCC -------------------> 3.3V/5V  |
   |-- GND -------------------> GND     |

[SOS Button]                       [ESP32]
   |                                    |
   |-- SOS_BUTTON_PIN --------------> GPIO 13 |
   |-- Button other pin  -----------> GND     |
