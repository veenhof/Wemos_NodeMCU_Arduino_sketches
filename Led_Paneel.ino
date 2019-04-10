// Uncomment this to enable Arduino OTA
#define UseOTA

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#ifdef UseOTA
  #include <ArduinoOTA.h>
  #include <WiFiUdp.h>
  #include <ESP8266mDNS.h>
#endif

// Neopixel
#define PIN            5
#define NUMPIXELS      100
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
int ledArray[NUMPIXELS][3]; // Custom array to allow us to buffer changes before sending them

// Wifi
WiFiClient espClient;
char* SSID = "veenhof-ddwrt";
char* WiFiPassword = "100%Geheim";
char* WiFiHostname = "Led_Paneel_Woonkamer";

// MQTT
const char* mqtt_server = "192.168.2.175";
int mqtt_port = 1883;
const char* mqttUser = "openhabian";
const char* mqttPass = "100%Volkswagen";
PubSubClient client(espClient);

const uint8_t bufferSize = 20;
char buffer[bufferSize];

// Reconnect Variables
unsigned long reconnectStart = 0;
unsigned long lastReconnectMessage = 0;
unsigned long messageInterval = 1000;
int currentReconnectStep = 0;
boolean offlineMode = true;
boolean recovered = false;

// Device Specific Topics
const char* commandlTopic = "light1/test";
const char* stateTopic = "light1/test/state";
const char* rgbCommandTopic = "light1/test/rgb";
const char* rgbStateTopic = "light1/test/rgb/state";
const char* availabilityTopic = "light1/test/availability";
const char* recoveryTopic = "light1/test/recovery";

// Device specific variables (currently only used for animations specific to my stair LEDs, leave false if you are not me)
boolean stairs = false;
int stairPixelArray[13];
int stairPixelArrayLength = 13;

// Sun Position
const char* sunPositionTopic = "sunPosition"; // This will be sent as a retained message so will be updated upon boot
int sunPosition = 0;

// Other
unsigned long availabilityPublishTimer = 0;
int availabilityPublishInterval = 30000;

#pragma region Global Animation Variables
const int numModes = 11; // This is the total number of modes
int currentMode; // 0

int brightness = 255;

int rgbValueOne[3]; // 1
int rgbValueTwo[3]; // 2

unsigned long colourDelay; // 3
int colourJump; // 4

unsigned long pixelDelay; // 5
int pixelJump; // 6

boolean randomise; // 7

unsigned long loopDelay; // 8
float highPixelDelay; // 9
float multiplier; // 10
int chance; // 11

int trailLength; // 12

unsigned long previousMillis;
boolean flipFlop;
int currentStep;
int currentPixel;

unsigned long colourDelaySeed;
int colourJumpSeed;
unsigned long pixelDelaySeed;
unsigned long loopDelaySeed;
float highPixelDelaySeed;
float multiplierSeed;
#pragma endregion

void resetGlobalAnimationVariables() {
  currentMode = 0;

  for (int colour = 0;colour < 3;colour ++) {
    rgbValueOne[colour] = 0;
    rgbValueTwo[colour] = 255;
  }

  colourDelay = 0;
  colourJump = 1;

  pixelDelay = 0;
  pixelJump = 1;

  randomise = false;

  loopDelay = 0;
  highPixelDelay = 0;
  multiplier = 1;
  
  previousMillis = 0;
  flipFlop = 0;
  currentStep = 0;
  currentPixel = 0;

  trailLength = 0;

  colourDelaySeed = 0;
  colourJumpSeed = 1;
  pixelDelaySeed = 0;
  loopDelaySeed = 0;
  highPixelDelaySeed = 0;
  multiplierSeed = 0;

  chance = 100;
}

void setModeDefaults(int mode) {
  // All Off
    // No additional defaults

  // All On
  if (mode == 1) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 147;
    rgbValueTwo[2] = 41;
    colourDelay = 2;
  }

  // Fade To Colour
  else if (mode == 2) {
    rgbValueTwo[0] = 0;
    rgbValueTwo[1] = 0;
    rgbValueTwo[2] = 255;
    colourDelay = 20;
  }

  // Strobe
  else if (mode == 3) {
    colourDelay = 40;
  }

  // Fire
  else if (mode == 4) {
    colourDelay = 20;
  }

  // Colour Phase
  else if (mode == 5) {
    colourDelay = 30;
  }

  // Sparkle
  else if (mode == 6) {
    rgbValueTwo[0] = 150;
    rgbValueTwo[1] = 150;
    rgbValueTwo[2] = 150;
    colourJump = 5;
  }

  // Shoot
  else if (mode == 7) {
    rgbValueTwo[0] = 50;
    rgbValueTwo[1] = 50;
    rgbValueTwo[2] = 255;
    colourJump = 20;
    loopDelay = 10000;
    highPixelDelay = 5;
    multiplier = 1.15;
    randomise = true;
    trailLength = 7;
  }

  // Mode 8
  else if (mode == 8) {
    rgbValueTwo[0] = 100;
    rgbValueTwo[1] = 100;
    rgbValueTwo[2] = 255;
    colourDelay = 4;
    chance = 15000;
  }

  // Stairs On
  else if (mode == 9) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 147;
    rgbValueTwo[2] = 41;
  }

  // Stair Shutdown
  else if (mode == 10) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 0;
    rgbValueTwo[2] = 0;
  }

  // Stair Startup
  else if (mode == 11) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 147;
    rgbValueTwo[2] = 41;
  }
}

void reconnect() {
  // IF statements used to complete process in single loop if possible
  
  // 0 - Turn the LED on and log the reconnect start time
  if (currentReconnectStep == 0) {
    digitalWrite(LED_BUILTIN, LOW);
    reconnectStart = millis();
    currentReconnectStep++;

   // If the ESP is reconnecting after a connection failure then wait a second before starting the reconnect routine
    if (offlineMode == false) {
      delay(1000);
    }
  }

  // If we've previously had a connection and have been trying to connect for more than 2 minutes then restart the ESP.
  // We don't do this if we've never had a connection as that means the issue isn't temporary and we don't want the relay
  // to turn off every 2 minutes.
  if (offlineMode == false && ((millis() - reconnectStart) > 120000)) {
    Serial.println("Restarting!");
    ESP.restart();
  }

  // 1 - Check WiFi Connection
  if (currentReconnectStep == 1) {
    if (WiFi.status() != WL_CONNECTED) {
      if ((millis() - lastReconnectMessage) > messageInterval) {
        Serial.print("Awaiting WiFi Connection (");
        Serial.print((millis() - reconnectStart) / 1000);
        Serial.println("s)");
        lastReconnectMessage = millis();
      }
    }
    else {
      Serial.println("WiFi connected!");
      Serial.print("SSID: ");
      Serial.print(WiFi.SSID());
      Serial.println("");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      Serial.println("");

      lastReconnectMessage = 0;
      currentReconnectStep = 2;
    }
  }

  // 2 - Check MQTT Connection
  if (currentReconnectStep == 2) {
    if (!client.connected()) {
      if ((millis() - lastReconnectMessage) > messageInterval) {
        Serial.print("Awaiting MQTT Connection (");
        Serial.print((millis() - reconnectStart) / 1000);
        Serial.println("s)");
        lastReconnectMessage = millis();

        String clientId = "Led_Lamp_Woonkamer";
        clientId += String(random(0xffff), HEX);
        client.connect(clientId.c_str(), mqttUser, mqttPass, availabilityTopic, 0, true, "0");
      }

      // Check the MQTT again and go forward if necessary
      if (client.connected()) {
        Serial.println("MQTT connected!");
        Serial.println("");

        lastReconnectMessage = 0;
        currentReconnectStep = 3;
      }

      // Check the WiFi again and go back if necessary
      else if (WiFi.status() != WL_CONNECTED) {
        currentReconnectStep = 1;
      }
    }
    else {
      Serial.println("MQTT connected!");
      Serial.println("");

      lastReconnectMessage = 0;
      currentReconnectStep = 3;
    }
  }

  // 3 - All connected, turn the LED back on and then subscribe to the MQTT topics
  if (currentReconnectStep == 3) {
    digitalWrite(LED_BUILTIN, HIGH);  // Turn the LED off

    publishAvailability();

    // MQTT Subscriptions
    client.subscribe(sunPositionTopic);
    client.subscribe(rgbCommandTopic);
    client.subscribe(commandlTopic);
    client.subscribe(recoveryTopic);

Serial.println(recoveryTopic);
Serial.println(commandlTopic);
Serial.println(rgbCommandTopic);
Serial.println(sunPositionTopic);
    
    if (offlineMode == true) {
      offlineMode = false;
      Serial.println("Offline Mode Deactivated");
      Serial.println("");
    }

    currentReconnectStep = 0; // Reset
  }
}

#pragma region MiscFunctions
int* randomColour() { // Not yet used but could be useful
  static int aRandomColour[3];

  aRandomColour[0] = random(255);
  aRandomColour[1] = random(255);
  aRandomColour[2] = random(255);

  return aRandomColour;
}

void updateLedArray_singleColour(int targetColour[3]) {
  for (int i = 0; i < NUMPIXELS; i++) {
    ledArray[i][0] = targetColour[0];
    ledArray[i][1] = targetColour[1];
    ledArray[i][2] = targetColour[2];
  }
}

void updateStripFromLedArray() {
  for (int i = 0; i < NUMPIXELS; i++) {
    pixels.setPixelColor(i, pixels.Color(ledArray[i][0], ledArray[i][1], ledArray[i][2]));
  }
  pixels.show();
}

void publishState() {
  if (currentMode == 0) {
    client.publish(stateTopic, "0", true);
  }
  else {
    client.publish(stateTopic, "1", true);
  }
}

void publishColour() {
  snprintf(buffer, bufferSize, "%d,%d,%d", rgbValueTwo[0], rgbValueTwo[1], rgbValueTwo[2]);
  client.publish(rgbStateTopic, buffer);
}

void publishAvailability() {
  client.publish(availabilityTopic, "1", true);
}

void publishRecovery() {
  DynamicJsonBuffer jsonBuffer;                   // Reserve memory space (https://bblanchon.github.io/ArduinoJson/)
  JsonObject& root = jsonBuffer.createObject();   // Create the JSON object

  // Always publish the mode so that there is a current retained message
  root["0"] = currentMode;

  // Only publish the other variables if they are non-default
  if (rgbValueOne[0] != 0 | rgbValueOne[1] != 0 | rgbValueOne[2] != 0) {
    JsonArray& rgbValueOneArray = root.createNestedArray("1");
    rgbValueOneArray.add(rgbValueOne[0]);
    rgbValueOneArray.add(rgbValueOne[1]);
    rgbValueOneArray.add(rgbValueOne[2]);
  }

  if (rgbValueTwo[0] != 255 | rgbValueTwo[1] != 255 | rgbValueTwo[2] != 255) {
    JsonArray& rgbValueTwoArray = root.createNestedArray("2");
    rgbValueTwoArray.add(rgbValueTwo[0]);
    rgbValueTwoArray.add(rgbValueTwo[1]);
    rgbValueTwoArray.add(rgbValueTwo[2]);
  }

  if (colourDelay != 0) {
    root["3"] = colourDelay;
  }

  if (colourJump != 1) {
    root["4"] = colourJump;
  }

  if (pixelDelay != 0) {
    root["5"] = pixelDelay;
  }

  if (pixelJump != 1) {
    root["6"] = pixelJump;
  }

  if (randomise != false) {
    root["7"] = randomise;
  }

  if (loopDelay != 0) {
    root["8"] = loopDelay;
  }

  if (highPixelDelay != 0) {
    root["9"] = highPixelDelay;
  }

  if (multiplier != 1) {
    root["10"] = multiplier;
  }

  if (chance != 100) {
    root["11"] = chance;
  }

  if (trailLength != 0) {
    root["12"] = trailLength;
  }

  char buffer[256];
  root.printTo(buffer, sizeof(buffer));
  client.publish(recoveryTopic, buffer, true);
}

void publishAll() {
  publishColour();
  publishState();
  publishRecovery();
  publishAvailability();
}
#pragma endregion

#pragma region Modes
void allRgbValueOne() {
  if (flipFlop == 0) { // This is used
    updateLedArray_singleColour(rgbValueOne);
    updateStripFromLedArray();
    flipFlop = 1;
  }
}

void allRgbValueTwo() {
  if (flipFlop == 0) { // This is used
    updateLedArray_singleColour(rgbValueTwo);
    updateStripFromLedArray();
    flipFlop = 1;
  }
}

void stairsOn() {
  if (flipFlop == 0) { // This isn't used
    updateLedArray_singleColour(rgbValueOne);

    for (int i = 0; i < 3; i++) {
      ledArray[3][i] = rgbValueTwo[i];
      ledArray[12][i] = rgbValueTwo[i];
      ledArray[21][i] = rgbValueTwo[i];
      ledArray[30][i] = rgbValueTwo[i];
      ledArray[40][i] = rgbValueTwo[i];
      ledArray[49][i] = rgbValueTwo[i];
      ledArray[58][i] = rgbValueTwo[i];
      ledArray[68][i] = rgbValueTwo[i];
      ledArray[77][i] = rgbValueTwo[i];
      ledArray[86][i] = rgbValueTwo[i];
      ledArray[96][i] = rgbValueTwo[i];
      ledArray[105][i] = rgbValueTwo[i];
      ledArray[111][i] = rgbValueTwo[i];
    }
    updateStripFromLedArray();
  }
}

void stairsOff() {
  if (flipFlop == 0) { // This isn't used
    updateLedArray_singleColour(rgbValueOne);

    if (sunPosition == 0) {
      rgbValueTwo[0] = 2;
      rgbValueTwo[1] = 1;
      rgbValueTwo[2] = 0;
    }
    else if (sunPosition == 1) {
      rgbValueTwo[0] = 0;
      rgbValueTwo[1] = 0;
      rgbValueTwo[2] = 0;
    }

    for (int i = 0; i < 3; i++) {
      ledArray[0][i] = rgbValueTwo[i];
      ledArray[9][i] = rgbValueTwo[i];
      ledArray[18][i] = rgbValueTwo[i];
      ledArray[27][i] = rgbValueTwo[i];
      ledArray[37][i] = rgbValueTwo[i];
      ledArray[46][i] = rgbValueTwo[i];
      ledArray[55][i] = rgbValueTwo[i];
      ledArray[65][i] = rgbValueTwo[i];
      ledArray[74][i] = rgbValueTwo[i];
      ledArray[83][i] = rgbValueTwo[i];
      ledArray[93][i] = rgbValueTwo[i];
      ledArray[102][i] = rgbValueTwo[i];
      ledArray[111][i] = rgbValueTwo[i];
    }
    updateStripFromLedArray();
  }
}

void fadeToColour(bool useFlipFlop = false) {
  if (flipFlop == 0) { // This is used
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= colourDelay) {
      int count = 0; // Reset the count
      for (int pixel = 0;pixel < NUMPIXELS;pixel ++){               // For every pixel
        if (ledArray[pixel][0] != rgbValueTwo[0] | ledArray[pixel][1] != rgbValueTwo[1] | ledArray[pixel][2] != rgbValueTwo[2]) {
          for (int colour = 0;colour < 3;colour++) {                    // For each colour
            if (ledArray[pixel][colour] < rgbValueTwo[colour]) {          // If the colour value is less that it should be
              ledArray[pixel][colour] += 1;                                 // Add 1
            }
            else if (ledArray[pixel][colour] > rgbValueTwo[colour]) {     // If the colour value is more that it should be
              ledArray[pixel][colour] -= 1;                                 // Subtract 1
            }
          }
      pixels.setPixelColor(pixel, pixels.Color(ledArray[pixel][0], ledArray[pixel][1], ledArray[pixel][2]));
        }
        else {
          count ++;
        }
      }
    pixels.show();
      if (count == NUMPIXELS && useFlipFlop == true) {
        flipFlop = 1;
      }
      
      previousMillis = currentMillis;
    }
  }
}

void fadePixelToTargetColour(int pixelNo) {
  if (millis() - previousMillis >= colourDelay) {
    if (ledArray[pixelNo][0] != rgbValueTwo[0] | ledArray[pixelNo][1] != rgbValueTwo[1] | ledArray[pixelNo][2] != rgbValueTwo[2]) {

      for (int colour = 0; colour < 3; colour++) {                    // For each colour
        if (ledArray[pixelNo][colour] < rgbValueTwo[colour]) {      // If the colour value is less that it should be
          ledArray[pixelNo][colour] += colourJump;                // Add the colour jump
          if (ledArray[pixelNo][colour] > rgbValueTwo[colour]) {  // Jump back if we've jumped over it
            ledArray[pixelNo][colour] = rgbValueTwo[colour];
          }
        }
        else if (ledArray[pixelNo][colour] > rgbValueTwo[colour]) { // If the colour value is more that it should be
          ledArray[pixelNo][colour] -= colourJump;                // Subtract the colour jump
          if (ledArray[pixelNo][colour] < rgbValueTwo[colour]) {  // Jump back if we've jumped under it
            ledArray[pixelNo][colour] = rgbValueTwo[colour];
          }
        }
      }

      pixels.setPixelColor(pixelNo, pixels.Color(ledArray[pixelNo][0], ledArray[pixelNo][1], ledArray[pixelNo][2]));
      pixels.show();
    }
    else {
      currentStep++;
    }

    previousMillis = millis();
  }
}

void fadePixelToBaseColour(int pixelNo) {
  if (millis() - previousMillis >= colourDelay) {
    if (ledArray[pixelNo][0] != rgbValueOne[0] | ledArray[pixelNo][1] != rgbValueOne[1] | ledArray[pixelNo][2] != rgbValueOne[2]) {

      for (int colour = 0; colour < 3; colour++) {                    // For each colour
        if (ledArray[pixelNo][colour] < rgbValueOne[colour]) {      // If the colour value is less that it should be
          ledArray[pixelNo][colour] += colourJump;                // Add the colour jump
          if (ledArray[pixelNo][colour] > rgbValueOne[colour]) {  // Jump back if we've jumped over it
            ledArray[pixelNo][colour] = rgbValueOne[colour];
          }
        }
        else if (ledArray[pixelNo][colour] > rgbValueOne[colour]) { // If the colour value is more that it should be
          ledArray[pixelNo][colour] -= colourJump;                // Subtract the colour jump
          if (ledArray[pixelNo][colour] < rgbValueOne[colour]) {  // Jump back if we've jumped under it
            ledArray[pixelNo][colour] = rgbValueOne[colour];
          }
        }
      }

      pixels.setPixelColor(pixelNo, pixels.Color(ledArray[pixelNo][0], ledArray[pixelNo][1], ledArray[pixelNo][2]));
      pixels.show();
    }
    else {
      currentStep++;
    }

    previousMillis = millis();
  }
}

void strobe() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= colourDelay) {
    if (flipFlop == 1) {
      updateLedArray_singleColour(rgbValueTwo);
      updateStripFromLedArray();
      flipFlop = !flipFlop;
    }
    else if (flipFlop == 0) {
      updateLedArray_singleColour(rgbValueOne);
      updateStripFromLedArray();
      flipFlop = !flipFlop;
    }
    previousMillis = currentMillis;
  }
}

void fire(int Cooling, int Sparking, int SpeedDelay) { // Need to tailor variables and update LED array
  static byte heat[NUMPIXELS];
  int cooldown;
  
  // Step 1.  Cool down every cell a little
  for( int i = 0; i < NUMPIXELS; i++) {
    cooldown = random(0, ((Cooling * 10) / NUMPIXELS) + 2);
    
    if(cooldown>heat[i]) {
      heat[i]=0;
    } else {
      heat[i]=heat[i]-cooldown;
    }
  }
  
  // Step 2.  Heat from each cell drifts 'up' and diffuses a little
  for( int k= NUMPIXELS - 1; k >= 2; k--) {
    heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
  }
    
  // Step 3.  Randomly ignite new 'sparks' near the bottom
  if( random(255) < Sparking ) {
    int y = random(7);
    heat[y] = heat[y] + random(160,255);
    //heat[y] = random(160,255);
  }

  // Step 4.  Convert heat to LED colors
  for( int j = 0; j < NUMPIXELS; j++) {
    setPixelHeatColor(j, heat[j] );
  }

  pixels.show();
  delay(SpeedDelay);
}
void setPixelHeatColor (int Pixel, byte temperature) {
  // Scale 'heat' down from 0-255 to 0-191
  byte t192 = round((temperature/255.0)*191);
 
  // calculate ramp up from
  byte heatramp = t192 & 0x3F; // 0..63
  heatramp <<= 2; // scale up to 0..252
 
  // figure out which third of the spectrum we're in:
  if( t192 > 0x80) {                     // hottest
    pixels.setPixelColor(Pixel, 255, 255, heatramp);
  } else if( t192 > 0x40 ) {             // middle
    pixels.setPixelColor(Pixel, 255, heatramp, 0);
  } else {                               // coolest
    pixels.setPixelColor(Pixel, heatramp, 0, 0);
  }
}
// Fire Source: https://www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/#fire

void colourPhase() {
  if (flipFlop == 1) { // fadeToColour will set flipFlop to 1 when complete so we use this to update the current step then reset the flipFlop
    currentStep++;
    flipFlop = 0;
    if (currentStep > 5) { // If the current step is greater than the number of steps we reset
      currentStep = 0;
    }
  }
  
  if (currentStep == 0) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 0;
    rgbValueTwo[2] = 0;
    fadeToColour(true);
  }
  else if (currentStep == 1) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 255;
    rgbValueTwo[2] = 0;
    fadeToColour(true);
  }
  else if (currentStep == 2) {
    rgbValueTwo[0] = 0;
    rgbValueTwo[1] = 255;
    rgbValueTwo[2] = 0;
    fadeToColour(true);
  }
  else if (currentStep == 3) {
    rgbValueTwo[0] = 0;
    rgbValueTwo[1] = 255;
    rgbValueTwo[2] = 255;
    fadeToColour(true);
  }
  else if (currentStep == 4) {
    rgbValueTwo[0] = 0;
    rgbValueTwo[1] = 0;
    rgbValueTwo[2] = 255;
    fadeToColour(true);
  }
  else if (currentStep == 5) {
    rgbValueTwo[0] = 255;
    rgbValueTwo[1] = 255;
    rgbValueTwo[2] = 255;
    fadeToColour(true);
  }
}

void sparkle() { 
  unsigned long currentMillis = millis();
  
  // Initiator
  if (currentStep == 0) { 
    allRgbValueOne(); // Sets all LEDs to rgbValueOne
    colourDelaySeed = colourDelay;
    colourJumpSeed = colourJump;
    pixelDelaySeed = pixelDelay;
    currentStep ++;
  }

  // Pixel Delay
  if (currentStep == 1) {
    if (currentMillis - previousMillis >= pixelDelay) {
      if (randomise == true) {
        pixelDelay = random(pixelDelaySeed);
        colourDelay = random(colourDelaySeed); // Only change between pixels so the light doesn't waver
        colourJump = random(1,colourJumpSeed); // Min of 1 otherwise there's a chance the loop will stop
      }
      previousMillis = currentMillis;
      currentStep ++;
    }
  }

  // Roll Up
  if (currentStep == 2 && (currentMillis - previousMillis >= colourDelay )) {
    if (ledArray[currentPixel][0] != rgbValueTwo[0] | ledArray[currentPixel][1] != rgbValueTwo[1] | ledArray[currentPixel][2] != rgbValueTwo[2]) {   // If the pixel hasn't reached peak
      for (int i = 1;!(i > colourJump);i++) {
        for (int col = 0;col < 3;col++) {                                                                         // For each colour
          if (ledArray[currentPixel][col] < rgbValueTwo[col]) {                                                   // If the colour value is less that it should be
            ledArray[currentPixel][col] += 1;                                                                     // Add 1
          }
          else if (ledArray[currentPixel][col] > rgbValueTwo[col]) {                                              // If the colour value is more that it should be
            ledArray[currentPixel][col] -= 1;                                                                     // Subtract 1
          }
        }
      }
      updateStripFromLedArray();
    }
    else {
      currentStep ++;
    }
  previousMillis = currentMillis;
  }

  // Roll Down
  if (currentStep == 3 && (currentMillis - previousMillis >= colourDelay )) {
    if (ledArray[currentPixel][0] != rgbValueOne[0] | ledArray[currentPixel][1] != rgbValueOne[1] | ledArray[currentPixel][2] != rgbValueOne[2]) {   // If the pixel hasn't reached base
      for (int i = 1;!(i > colourJump);i++) {
        for (int col = 0;col < 3;col++) {                                                                         // For each colour
          if (ledArray[currentPixel][col] < rgbValueOne[col]) {                                                   // If the colour value is less that it should be
            ledArray[currentPixel][col] += 1;                                                                     // Add 1
          }
          else if (ledArray[currentPixel][col] > rgbValueOne[col]) {                                              // If the colour value is more that it should be
            ledArray[currentPixel][col] -= 1;                                                                     // Subtract 1
          }
        }
      }
      updateStripFromLedArray();
    }
    else {
      currentStep = 1; // Go back to the mid pixel wait
      currentPixel = random(NUMPIXELS);
    }
  previousMillis = currentMillis;
  }
}

void rain() {
  unsigned long currentMillis = millis();

  if (random(chance) == 1) {
    int randomPixel = random(NUMPIXELS);
    ledArray[randomPixel][0] = rgbValueTwo[0];
    ledArray[randomPixel][1] = rgbValueTwo[1];
    ledArray[randomPixel][2] = rgbValueTwo[2];
    updateStripFromLedArray();
  }

  if (currentMillis - previousMillis >= colourDelay) {
    for (int pixel = 0;pixel < NUMPIXELS;pixel ++){               // For every pixel
      for (int i = 1;!(i > colourJump);i++) {
        if (ledArray[pixel][0] != rgbValueOne[0] | ledArray[pixel][1] != rgbValueOne[1] | ledArray[pixel][2] != rgbValueOne[2]) {
          for (int colour = 0;colour < 3;colour++) {                    // For each colour
            if (ledArray[pixel][colour] < rgbValueOne[colour]) {          // If the colour value is less that it should be
              ledArray[pixel][colour] += 1;                                 // Add 1
            }
            else if (ledArray[pixel][colour] > rgbValueOne[colour]) {     // If the colour value is more that it should be
              ledArray[pixel][colour] -= 1;                                 // Subtract 1
            }
          }
          pixels.setPixelColor(pixel, pixels.Color(ledArray[pixel][0],ledArray[pixel][1],ledArray[pixel][2]));
        }
      }
    }
    updateStripFromLedArray();
    previousMillis = currentMillis;
  }
}

void shoot() {
  unsigned long currentMillis = millis();

  // Initiator
  if (currentStep == 0) {
    allRgbValueOne(); // Sets all LEDs to rgbValueOne
    loopDelaySeed = loopDelay;
    colourJumpSeed = colourJump;
    multiplierSeed = multiplier;
    highPixelDelaySeed = highPixelDelay; // Seeded because it's multiplied not because it's randomised
    currentStep++;
  }

  // Loop Delay
  if (currentStep == 1) {
    if (currentMillis - previousMillis >= loopDelay) {
      if (randomise == true) {
        loopDelay = random(loopDelaySeed);
        colourJump = random(1, colourJumpSeed);
        float multiplierSeedX = (multiplier * 100.00);
        multiplier = (random(multiplierSeedX * 0.95, multiplierSeedX * 1.05)) / 100.00; // +-5% - Best at 1.25
      }
      highPixelDelay = highPixelDelaySeed;
      previousMillis = currentMillis;
      currentStep++;
    }
  }

  // Pixel Delay
  if (currentStep == 2) {
    if (currentMillis - previousMillis >= pixelDelay) {
      previousMillis = currentMillis;
      currentStep++;
    }
  }

  // Roll Up
  if (currentPixel > (NUMPIXELS - 1)) { // Skip the roll up if we're after the last pixel (trail)
    currentStep = 4;
  }

  if (currentStep == 3 && (currentMillis - previousMillis >= colourDelay)) {
    if (ledArray[currentPixel][0] != rgbValueTwo[0] | ledArray[currentPixel][1] != rgbValueTwo[1] | ledArray[currentPixel][2] != rgbValueTwo[2]) {   // If the pixel hasn't reached peak
      for (int i = 1; !(i > colourJump); i++) {
        for (int col = 0; col < 3; col++) {                                                                         // For each colour
          if (ledArray[currentPixel][col] < rgbValueTwo[col]) {                                                   // If the colour value is less that it should be
            ledArray[currentPixel][col] += 1;                                                                   // Add 1
          }
          else if (ledArray[currentPixel][col] > rgbValueTwo[col]) {                                              // If the colour value is more that it should be
            ledArray[currentPixel][col] -= 1;                                                                   // Subtract 1
          }
        }
      }
      updateStripFromLedArray();
    }
    else {
      currentStep++;
    }
    previousMillis = currentMillis;
  }

  // High Pixel Delay
if (currentStep == 4) {
  if (currentMillis - previousMillis >= highPixelDelay) {
    previousMillis = currentMillis;
    highPixelDelay = (highPixelDelay * multiplier);
    currentStep++;
  }
}

// Roll Down
if (currentStep == 5 && (currentMillis - previousMillis >= colourDelay)) {
  if (trailLength == 0) {
    if (ledArray[currentPixel][0] != rgbValueOne[0] | ledArray[currentPixel][1] != rgbValueOne[1] | ledArray[currentPixel][2] != rgbValueOne[2]) {   // If the pixel hasn't reached base
      for (int i = 1; !(i > colourJump); i++) {
        for (int col = 0; col < 3; col++) {                                                                         // For each colour
          if (ledArray[currentPixel][col] < rgbValueOne[col]) {                                                   // If the colour value is less that it should be
            ledArray[currentPixel][col] += 1;                                                                   // Add 1
          }
          else if (ledArray[currentPixel][col] > rgbValueOne[col]) {                                              // If the colour value is more that it should be
            ledArray[currentPixel][col] -= 1;                                                                   // Subtract 1
          }
        }
      }
      updateStripFromLedArray();
    }
    else {
      if (currentPixel == (NUMPIXELS - 1)) {
        currentPixel = 0;
        currentStep = 1;
      }
      else {
        currentPixel++;
        currentStep = 2;
      }
    }
  }
  else {
    Serial.println("---");
    Serial.print("currentPixel = ");
    Serial.println(currentPixel);

    int currentTrailPixel = currentPixel; // Start with the current pixel as we're about to put the next one up

    while (currentTrailPixel >= (currentPixel - (trailLength + 1))) {

      if (currentTrailPixel >= 0 && currentTrailPixel < NUMPIXELS) {
        Serial.print("trailPixelToUpdate = ");
        Serial.println(currentTrailPixel);

        if (currentTrailPixel > (currentPixel - trailLength)) { // This is the trail
          int trailDifference = (currentPixel - currentTrailPixel);
          float trailPercentage = ((1.0 / trailLength) * (trailLength - trailDifference));
          Serial.print("trailPercentage = ");
          Serial.println(trailPercentage);

          ledArray[currentTrailPixel][0] = (rgbValueTwo[0] * trailPercentage);
          ledArray[currentTrailPixel][1] = (rgbValueTwo[1] * trailPercentage);
          ledArray[currentTrailPixel][2] = (rgbValueTwo[2] * trailPercentage);
        }
        else { // Clear the trail
          ledArray[currentTrailPixel][0] = rgbValueOne[0];
          ledArray[currentTrailPixel][1] = rgbValueOne[1];
          ledArray[currentTrailPixel][2] = rgbValueOne[2];
        }
      }

      currentTrailPixel = (currentTrailPixel - 1);
    }

    updateStripFromLedArray();

    if (currentPixel == ((NUMPIXELS - 1) + trailLength)) {
      currentPixel = 0;
      currentStep = 1;
    }
    else {
      currentPixel++;
      currentStep = 2;
    }
  }
  previousMillis = currentMillis;
}
}

void stairStartup() {
  if (currentStep == 0) {
    allRgbValueOne(); // Turn all the lights off first
    currentPixel = 12; // Start at the end
    currentStep++;
  }

  else if (currentStep == 1) {
    fadePixelToTargetColour(stairPixelArray[currentPixel]);
  }

  else if (currentStep == 2) {
    if ((currentPixel - 1) <= -1) { // If we've done all the pixels
      currentStep = 4;
    }

    else if (millis() - previousMillis >= pixelDelay) {
      currentPixel--;
      currentStep = 1;
    }
  }

  else if (currentStep == 4) {
    currentMode = 9; // stairsOn
    setModeDefaults(currentMode);
    
    // No need to publish state as mode 10 is considered on anyway and so has already been published
    publishColour();
    publishRecovery();
  }
}

void stairShutdown() {
  // This step fades the top LED to red to indicate shutdown
  if (currentStep == 0) {
    fadePixelToTargetColour(111);
  }
  
  else if (currentStep == 1) {
    fadePixelToBaseColour(stairPixelArray[currentPixel]);
  }

  else if (currentStep == 2) {
    if ((currentPixel + 1) >= stairPixelArrayLength) { // If we've done all the pixels
      currentStep = 3;
    }

    else if (millis() - previousMillis >= pixelDelay) {
      currentPixel++;
      currentStep = 1;
    }
  }

  else if (currentStep == 3) {
    currentMode = 0; // Off
    setModeDefaults(currentMode);
    publishAll();
  }
}
#pragma endregion

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);     // Initialize the LED_BUILTIN pin as an output
  digitalWrite(LED_BUILTIN, LOW);   // Turn the LED on to indicate no connection
  
  Serial.begin(115200);

  pixels.begin(); // This initializes the NeoPixel library.
  pixels.setBrightness(brightness);
  pixels.show();

  WiFi.hostname(WiFiHostname);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, WiFiPassword);
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // Initialise the custom ledArray library
  for (int i = 0;i < NUMPIXELS;i ++) {
    ledArray[i][0] = 0;
    ledArray[i][1] = 0;
    ledArray[i][2] = 0;
  }

  // Initialise the stairPixelArray
  stairPixelArray[0] = 3;
  stairPixelArray[1] = 12;
  stairPixelArray[2] = 21;
  stairPixelArray[3] = 30;
  stairPixelArray[4] = 40;
  stairPixelArray[5] = 49;
  stairPixelArray[6] = 58;
  stairPixelArray[7] = 68;
  stairPixelArray[8] = 77;
  stairPixelArray[9] = 86;
  stairPixelArray[10] = 96;
  stairPixelArray[11] = 105;
  stairPixelArray[12] = 111;

  // Set all global animation variables to their defaults
  resetGlobalAnimationVariables();

  // OTA
  #ifdef UseOTA
    ArduinoOTA.setHostname(WiFiHostname);
    
    ArduinoOTA.onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else { // U_SPIFFS
        type = "filesystem";
      }
  
      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        Serial.println("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        Serial.println("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        Serial.println("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        Serial.println("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        Serial.println("End Failed");
      }
    });
    ArduinoOTA.begin();
  #endif
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println("> MQTT Received");

#pragma region commandlTopic
  if (String(commandlTopic).equals(topic)) {
    if (length == 1) {
      // Off
      if ((char)payload[0] == '0') {
        resetGlobalAnimationVariables();
        currentMode = 0; // Not required as 0 is set by the reset, left in for clarity
        setModeDefaults(currentMode);
        publishState();
        publishRecovery();
      }
      // On
      else if ((char)payload[0] == '1') {
        // Only turn on if currentMode is 0
        if (currentMode == 0) {
          resetGlobalAnimationVariables();
          currentMode = 1;
          setModeDefaults(currentMode);
          publishAll(); // State + Colour + Recovery
        }
      }
      // Toggle - Using a re-publish ensures any changes to the On / Off routines are used for the toggle as well
      else if ((char)payload[0] == '2') {
        if (currentMode == 0) {
          client.publish(commandlTopic, "1");
        }
        else {
          client.publish(commandlTopic, "0"); 
        }
      }
      // Cycle Modes
      else if ((char)payload[0] == '9') {
        int lastMode = currentMode;

        resetGlobalAnimationVariables();

        currentMode = ++lastMode;

        if (currentMode > numModes) {
          currentMode = 0;
        }

        setModeDefaults(currentMode);
        publishAll();
      }
    }

    else if ((char)payload[0] == '{') {       // It's a JSON
      char message_buff[256];               // Max of 256 characters
      for (int i = 0; i < length; i++) {
        message_buff[i] = payload[i];
        if (i == (length - 1)) {          // If we're at the last character
          message_buff[i + 1] = '\0';   // Add a string terminator to the next character
        }
      }  
      DynamicJsonBuffer jsonBuffer;                            // Step 1: Reserve memory space (https://bblanchon.github.io/ArduinoJson/)
      JsonObject& root = jsonBuffer.parseObject(message_buff); // Step 2: Deserialize the JSON string

      if (!root.success()) {
        Serial.println("parseObject() failed");
      }
      else if (root.success()) {
        if (root.containsKey("0")) {
          resetGlobalAnimationVariables(); // Only do this if the mode changes
          currentMode = root["0"];
          setModeDefaults(currentMode); // Only do this if the mode changes
          
          // We don't publish the recovery message here as other variables might have changed without the mode changing and so we wait until later
          publishState();
          publishColour();
        }
        if (currentMode > numModes) { // If mode is greater than is possible, set to 0
          currentMode = 0;
        }

        if (root.containsKey("1")) {
          rgbValueOne[0] = root["1"][0];
          rgbValueOne[1] = root["1"][1];
          rgbValueOne[2] = root["1"][2];
                  flipFlop = 0; // When displaying a static colour this is set to 1 once the LED strip has been updated, resetting this to 0 is needed to trigger the strip update again now the colour has changed.
        }

        if (root.containsKey("2")) {
          rgbValueTwo[0] = root["2"][0];
          rgbValueTwo[1] = root["2"][1];
          rgbValueTwo[2] = root["2"][2];
                  flipFlop = 0; // When displaying a static colour this is set to 1 once the LED strip has been updated, resetting this to 0 is needed to trigger the strip update again now the colour has changed.
          publishColour();
        }

        if (root.containsKey("3")) {
          colourDelay = root["3"];
        }

        if (root.containsKey("4")) {
          colourJump = root["4"];
        }

        if (root.containsKey("5")) {
          pixelDelay = root["5"];
        }

        if (root.containsKey("6")) {
          pixelJump = root["6"];
        }

        if (root.containsKey("7")) {
          randomise = root["7"];
        }

        if (root.containsKey("8")) {
          loopDelay = root["8"];
        }

        if (root.containsKey("9")) {
          highPixelDelay = root["9"];
        }

        if (root.containsKey("10")) {
          multiplier = root["10"];
        }

        if (root.containsKey("11")) {
          chance = root["11"];
        }

        if (root.containsKey("12")) {
          trailLength = root["12"];
        }

      }

      publishRecovery();
    }
  }
#pragma endregion

#pragma region rgbCommandTopic
  else if (String(rgbCommandTopic).equals(topic)) {
    String payloadString;
    for (uint8_t i = 0; i < length; i++) {
      payloadString.concat((char)payload[i]);
    }

    uint8_t firstIndex = payloadString.indexOf(',');
    uint8_t lastIndex = payloadString.lastIndexOf(',');

    uint8_t rgb_red = payloadString.substring(0, firstIndex).toInt();
    if (rgb_red < 0 || rgb_red > 255) {
      return;
    }
    else {
      rgbValueTwo[0] = rgb_red;
    }

    uint8_t rgb_green = payloadString.substring(firstIndex + 1, lastIndex).toInt();
    if (rgb_green < 0 || rgb_green > 255) {
      return;
    }
    else {
      rgbValueTwo[1] = rgb_green;
    }

    uint8_t rgb_blue = payloadString.substring(lastIndex + 1).toInt();
    if (rgb_blue < 0 || rgb_blue > 255) {
      return;
    }
    else {
      rgbValueTwo[2] = rgb_blue;
    }
        
    flipFlop = 0; // When displaying a static colour this is set to 1 once the LED strip has been updated, resetting this to 0 is needed to trigger the strip update again now the colour has changed.

    // No need to publish the state, it can't have changed.
    publishColour();
    publishRecovery();
  }
#pragma endregion

#pragma region sunPositionTopic
  else if (String(sunPositionTopic).equals(topic)) {
    if ((char)payload[0] == '0') {
      sunPosition = 0;
    }
    else if ((char)payload[0] == '1') {
      sunPosition = 1;
    }
  }
#pragma endregion

#pragma region recoveryTopic
  else if (String(recoveryTopic).equals(topic) && recovered == false) {
    char message_buff[256];
    for (int i = 0; i < length; i++) {
      message_buff[i] = payload[i];
      if (i == (length - 1)) {              // If we're at the last character
        message_buff[i + 1] = '\0';       // Add a string terminator to the next character
      }
    }
    StaticJsonBuffer<300> jsonBuffer;             // Reserve memory space (https://bblanchon.github.io/ArduinoJson/)
    JsonObject& root = jsonBuffer.parseObject(message_buff);    // Step 2: Deserialize the JSON string

    if (!root.success()) {
      Serial.println("parseObject() failed");
    }
    else if (root.success()) {
      if (root.containsKey("0")) {
        currentMode = root["0"];
        setModeDefaults(currentMode); // Only do this if the mode changes
      }
      if (currentMode > numModes) { // If mode is greater than is possible, set to 0
        currentMode = 0;
      }

      if (root.containsKey("1")) {
        rgbValueOne[0] = root["1"][0];
        rgbValueOne[1] = root["1"][1];
        rgbValueOne[2] = root["1"][2];
      }

      if (root.containsKey("2")) {
        rgbValueTwo[0] = root["2"][0];
        rgbValueTwo[1] = root["2"][1];
        rgbValueTwo[2] = root["2"][2];
        publishColour();
      }

      if (root.containsKey("3")) {
        colourDelay = root["3"];
      }

      if (root.containsKey("4")) {
        colourJump = root["4"];
      }

      if (root.containsKey("5")) {
        pixelDelay = root["5"];
      }

      if (root.containsKey("6")) {
        pixelJump = root["6"];
      }

      if (root.containsKey("7")) {
        randomise = root["7"];
      }

      if (root.containsKey("8")) {
        loopDelay = root["8"];
      }

      if (root.containsKey("9")) {
        highPixelDelay = root["9"];
      }

      if (root.containsKey("10")) {
        multiplier = root["10"];
      }

      if (root.containsKey("11")) {
        chance = root["11"];
      }

      if (root.containsKey("12")) {
        trailLength = root["12"];
      }

      // No need to publish recovery message as it would match the current settings anyway
      publishState();
      publishColour();

      flipFlop = 0; // When displaying a static colour this is set to 1 once the LED strip has been updated, resetting this to 0 is needed to trigger the strip update again.
    }

    recovered = true;
  }
#pragma endregion
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  else {
    client.loop();
  }

  if (currentMode == 0) {
    if (stairs == true) { // Special Off Mode for Stairs
      stairsOff();
    }
    else {
      allRgbValueOne(); // Normal
    }
  }
  else if (currentMode == 1) {
  fadeToColour(true);
  }
  else if (currentMode == 2) {
    fadeToColour(true);
  }
  else if (currentMode == 3) {
    strobe();
  }
  else if (currentMode == 4) {
    fire(55,120,colourDelay);
  }
  else if (currentMode == 5) {
    colourPhase();
  }
  else if (currentMode == 6) {
    sparkle();
  }
  else if (currentMode == 7) {
    shoot();
  }
  else if (currentMode == 8) {
    rain();
  }
  else if (currentMode == 9) {
  stairsOn();
  }
  else if (currentMode == 10) {
  stairShutdown();
  }
  else if (currentMode == 11) {
  stairStartup();
  }

  if ((millis() - availabilityPublishInterval) >= availabilityPublishTimer) {
    publishAvailability();
    availabilityPublishTimer = millis();
  }

  #ifdef UseOTA
    ArduinoOTA.handle();
  #endif

  yield();
}
