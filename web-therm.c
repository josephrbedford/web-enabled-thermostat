/*
Web connected thermostat.
Joe Bedford.
Latest update 03-28-2020 - modified setpoint to float from int.
Uses WifiManager by Tzapu (https://github.com/tzapu/WiFiManager) for wifi config.
Written for Wemos D1 development board. With minor changes will run on wifi arduino.
*/

// DEFINES ---------------------------

#define TEMPFRQ 1000                  // Freq of temperature reading
#define THERMFRQ 5000                 // Freq of thermo on off descision
#define TEMPARRAYSIZE 60              // Size of avg array for temp calc
#define POWER_WAIT 300000             // Time to wait to restart cooler (5 mins)

// INCLUDES ---------------------------

#include <ESP8266WiFi.h>              // ESP8266 Core WiFi Library
#include <DNSServer.h>                // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>         // Local WebServer used to serve the configuration portal
#include <WiFiManager.h>              // WiFi Configuration Magic
#include <EEPROM.h>                   // Enables reading and writing EEPROM

// SCHEDULING TASKS ----------------

unsigned long prevTempMillis = 0;   // Init timer var for Temp readings
unsigned long prevThermoMillis = 0; // Init timer var for Thermostat

// READING TEMP --------------------

float tempArray[TEMPARRAYSIZE];     // Define constant array for temp avg
int tempArrayCtr = 0;               // Counter for array

// THERMOSTAT ----------------------

float setPoint = 73.50;                // Startup setpoint for heat if nothing in EEPROM
float hyst = 0.05;                  // Hysterysis setting in degrees F
float avgTemp = 0;                  // Initialise avg temp
boolean device = 0;                 // Device on or off
boolean heatMode = 0;                // Heat or cool mode
boolean lastDeviceState = 0;        // Last device state (used so not writing to output pin unless necessary)
boolean heatOut = 0;                // Connects device to output if power on
boolean powerSet = 0;               // On off switch
boolean deviceLastSetting = 0;    // used to tell if first time off
int failSafeTemp = 1;             // Shut down if temp lower than this (sensor failure)

// PINS -----------------------------

int tempPin = A0;                   // Pin temp sensor is attached to
int heatCtrlPin = D2;               // Pin controlling device

// EEPROM ---------------------------

const int ID_ADDR = 0;              // the EEPROM address used to store the ID and show data present
const int setPointAddr = 5;         // address to begin storing settings
const byte EEPROM_ID = 0x99;        // used to identify if valid data in EEPROM
const byte EEPROM_CLR = 0x98;       // used to identify invalid data
int storedSetPoint = 0;             // var used to hold Temp setpoint from EEPROM
bool storedPowerState = 0;          // var used to hold Power setting from EEPROM
bool storedHeatMode = 0;            // var used to hold heat mode setting from EEPROM

// COOLER RESTART TIMER -------------
unsigned long shutDownTimer = 0;

ESP8266WebServer server(80);        // Start web server port 80

void setup(){

// CONNECT TO WIFI -------------------------------------

  WiFiManager wifiManager;          // Start WifiManager
  wifiManager.autoConnect();        // Connect to Wifi

// BEGIN SERVICES --------------------------------------

  Serial.begin(9600);               // Start serial service. Only using when debugging
  EEPROM.begin(512);                // Start EEPROM service

// SET PIN MODES --------------------------------------
  
  pinMode(tempPin, INPUT);          // Assign input pin for temp sensor
  pinMode(heatCtrlPin, OUTPUT);     // Assign output pin for Relay controlling heat



// ASSIGN WEBPAGES TO BUTTON PRESSES ----------------------

  server.on("/", handle_OnConnect);             // If nothing in header
  server.on("/settings", settings);             // Settings button clicked
  server.on("/addDegree", addDegree);           // If + button clicked
  server.on("/minusDegree", minusDegree);       // If - button clicked
  server.on("/powerOn", powerOn);               // If power on button clicked
  server.on("/powerOff", powerOff);             // If power off button clicked
  server.on("/modeHeat", modeHeat);             // If heat mode button clicked
  server.on("/modeCold", modeCold);             // If cold mode button clicked
  server.on("/writeEEPROM", writeEEPROM);       // If save button clicked
  server.on("/resetPage", resetPage);           // If back button pressed
  server.on("/eraseEEPROM", eraseEEPROM);       // If erase eeprom pressed
  server.onNotFound(handle_NotFound);           // If something else in header
  server.begin();


// CHECK FOR SETTINGS IN EEPROM ------------------------
  
  byte id = EEPROM.read(ID_ADDR);   // read the first byte from the EEPROM

  if( id == EEPROM_ID) {            // EEPROM contains saved settings
    
    Serial.println("Data found in eeprom");

    byte hiByte =  EEPROM.read(setPointAddr);
    byte lowByte =  EEPROM.read(setPointAddr+1);
    storedSetPoint =  word(hiByte, lowByte);




    // storedSetPoint = EEPROM.read(setPointAddr);     // Read first EEPROM addr and load var for EEPROM value with setpoint
      setPoint = storedSetPoint;                    // Load stored setpoint value to working setpoint
      setPoint /= 10;                               // Div by 10 to get correct float value back
      
    storedPowerState = EEPROM.read(setPointAddr+5); // Read second EEPROM addr and load var for EEPROM value with power setting
      powerSet = storedPowerState;                  // Load stored power value to working power setting

    storedHeatMode = EEPROM.read(setPointAddr+6);   // Read third EEPROM addr and load var for EEPROM value with heat setting
      heatMode = storedHeatMode;                  // Load stored heat value to working power setting


    Serial.println(storedSetPoint);
    Serial.println(setPoint);
  } else {
    Serial.println("Data not found in eeprom");
    
  }
}

void loop(){

// SCHEDULE TEMPERATURE READINGS -----------------------------
// Initiates temp reading every TEMPFRQ milliseconds
  
  if (millis() - prevTempMillis >= TEMPFRQ) {      
    prevTempMillis = millis();                          // Reset timer
    getTemp();                                          // Get temp from sensor (using function getTemp)

    // Serial.println("Pin Reading");
    // Serial.println(analogRead(tempPin));
  }

// SCHEDULE THERMOSTAT --------------------------------------
// Initiates thermostat decision every THERMFRQ milliseconds if power setting is on
  
/*  if (millis() - prevThermoMillis >= THERMFRQ) {
    Serial.println("Time until cooler start");
    Serial.println(millis() - shutDownTimer);
    Serial.println("Output Device");
    Serial.println(device);
   
      prevThermoMillis = millis();                  // Reset timer
    
     } */
    
  
  thermoStat();                                 // Run thermostat object

// CHECK CHANGES AND UPDATE OUTPUT PIN -----------------------
  sendOutput();                                 // Update output pin

// LISTEN FOR HTML CONNECTIONS ---------------------------------
  server.handleClient();                        // Waiting for html connection

 delay(10);
} 

// -------------------------------------------------------------------------------------------------------



void getTemp() {                               
// READS TEMP SENSOR, CONVERTS TO F, LOADS ARRAY AND TAKES AVERAGE -----------------
// Array is on infinite loop, restarts loading readings at 0 once full
// Used to reduce noise in temperature reading

  float voltage, degreesC, degreesF;            // Define floats for temp sensor
  
    voltage = getVoltage(tempPin);              // Calls getVoltage function to return voltage read from sensor
    degreesC = (voltage - 0.5) * 100.0;         // Convert voltage to degrees C
    
    degreesF = degreesC * (9.0/5.0) + 32.0;     // Convert from F to C
    
    tempArray[tempArrayCtr] = degreesF;         // Load array with temp
    
    avgTemp = averageArrayItem(tempArray, TEMPARRAYSIZE); // Take average of temps in array (uses averageArrayItem function)
    
    tempArrayCtr = tempArrayCtr + 1;            // Increment counter
    
    if (tempArrayCtr >= TEMPARRAYSIZE) {
      tempArrayCtr = 0;                         // Reset counter and array
    }
  }

void thermoStat() { 
// COMPARES TEMP TO SETPOINT AND CONTROLS DEVICE TAKING HYSTERISYS INTO ACCOUNT --------------
  if (avgTemp >= failSafeTemp) {                          // If warmer than 1 degree ie sensor working
    if (powerSet == 1) {                                  // If power is on
     if (heatMode == 0) {                                  // If heatMode set to 0 (heat)
       if (avgTemp <= setPoint - hyst) {                  // If colder than current setpoint - hysterysis
         device = 1;                                       // Request device on
       } else if (avgTemp >= setPoint) {                  // If hotter than current setpoint
          device = 0;                                       // Request device off
       } else {                                           // Anything else
          device = 0;                                       // Request device off
       }
       
   
    } else {                                              // If heatMode set to 1 (cool)
      if ((avgTemp >= setPoint + hyst) && ((millis() - shutDownTimer) >= POWER_WAIT))  {                  // If hotter than current setpoint + hysterysis and more than 5 mins
         device = 1;                                      // Request device on
       // deviceLastSetting = device;                      // Remember last setting
       
      } else if (avgTemp <= setPoint) {                  // If cooler than current setpoint
         device = 0;                                      // Request device off
         if (deviceLastSetting != device) {               // First iteration since shutdown - start timer
           shutDownTimer = millis();
       }
       }
    }
  } else {                                                // Power setting is off
        device = 0;
        if (deviceLastSetting != device) {               // First iteration since shutdown - start timer
           shutDownTimer = millis();
  }
  }
deviceLastSetting = device;                // Update state for next time
  
}
  else {
    Serial.println("Shut down heater, sensor failure.");
    if (device == 1) {
        device = 0;                                    // Shut off heater
  }
 }
}

void sendOutput() {
// COMPARES CURRENT DEVICE SETTING TO LAST SETTING AND UPDATES PIN IF CHANGED ---------------------
     
     if (device != lastDeviceState) {          // Compares current heat request to last known state of output
    digitalWrite(heatCtrlPin, device);      // Write output state to output pin
  
  }
   lastDeviceState = device;                // Update state for next time
 
}
   
float getVoltage(int pin) {
// READS PIN AND RETURNS VOLTAGE FOR TEMP SENSOR -------------------------
// Converts pin read (0-1024) to actual voltage returned from sensor in range 0-3.3v
   return (analogRead(pin) * 0.00302734375); // Used 3.1 div 1024 - seems more accutate on ESP8266
}

float averageArrayItem(float arr[], int n) {  
// RETURNS AVERAGE OF GIVEN ARRAY ---------------------------------------------
  int i;
  float sum = 0;
  
  for (int i=0; i<n; i++)
    sum += arr[i];
  
  return sum/n;
  
}

void handle_OnConnect() {
// CALLS MAIN WEBPAGE ------------------------------------------------------------                   
   server.send(200, "text/html", SendHTML(avgTemp, setPoint));  // Calls SendHTML to generate dynamic webpage
}

void addDegree() {
// ADDS .1 DEGREE TO SETPOINT AND CALLS REDIRECT TO MAIN WEBPAGE --------------------                              
  setPoint += 0.1;                                  // Increment temp setpoint
  server.send(200, "text/html", sendRedirect());  // Once increment done, resets webpage to root
  
}

void powerOn() {
// SETS POWER VAR TO ON AND CALLS REDIRECT TO MAIN WEBPAGE ------------------------                              
  powerSet = 1;                                   // Sets var for thermostat to turn on
  server.send(200, "text/html", sendRedirect());  // Resets webpage to root
  }

void powerOff() {
// SETS POWER VAR TO OFF AND CALLS REDIRECT TO MAIN WEBPAGE ------------------------                               
  powerSet = 0;                                  // Sets var for thermostat to turn off
  server.send(200, "text/html", sendRedirect());  // Resets webpage to root
}

void minusDegree() {  
// SUBTRACTS DEGREE AND CALLS REDIRECT TO MAIN WEBPAGE ------------------------------                            
  setPoint -= 0.1;                                  // Decrement temp setpoint
  server.send(200, "text/html", sendRedirect());  // Once decrement done, resets webpage to root
  
}
void modeHeat() {
// SETS SYSTEM TO HEATER MODE (heatMode false) ----------------------------------------
  heatMode = 0;                                    // Set var to heat mode
  server.send(200, "text/html", sendRedirect());  // Once mode changed, resets webpage to root
  
}

void modeCold() {
// SETS SYSTEM TO HEATER MODE (heatMode false) ----------------------------------------
  heatMode = 1;                                    // Set var to cold mode
  server.send(200, "text/html", sendRedirect());  // Once mode changed, resets webpage to root
  
}

void eraseEEPROM () {

  EEPROM.write(ID_ADDR,EEPROM_CLR);                  // write the ID to indicate invalid data
   
   EEPROM.commit();                                   // Commit changes to EEPROM

  server.send(200, "text/html", sendRedirect());  // Once mode changed, resets webpage to root
  
}

void writeEEPROM () {
// WRITES CURRENT SETPOINT AND POWER SETTING TO EEPROM -------------------------------
// Will not update if value being stored matches value known to be in EEPROM (saves write cycles)
  bool setPointUpdated = 0;
  bool powerUpdated = 0;
  bool heatModeUpdated = 0;
  int checkval = 0;
  
  if (storedSetPoint != setPoint * 10) {          // If stored setpoint value is different from working setpoint
    setPointUpdated = 1;                          // Set flag for update done
    storedSetPoint = setPoint * 10;                // Update stored setpoint to setpoint
    
    byte hiByte = highByte(storedSetPoint);
          byte loByte = lowByte(storedSetPoint);
          EEPROM.write(setPointAddr, hiByte);
          EEPROM.write(setPointAddr+1, loByte); 
    
    
    
    // EEPROM.write(setPointAddr, storedSetPoint);     // Write changes to EEPROM (not actually done until commit)
    Serial.println("Stored Setpoint");
    Serial.println(storedSetPoint);
    Serial.println("Set Point");
    Serial.println(setPoint);

  } else {
    setPointUpdated = 0;                          // Set flag for no update done
  }

  if (storedPowerState != powerSet) {             // If stored power value is different from working value
    powerUpdated = 1;                             // Set flag for update done
    storedPowerState = powerSet;                  // Update stored power state to working power state
    EEPROM.write(setPointAddr+5, storedPowerState); // Write changes to EEPROM (not done until commit)
   
   } else {
    powerUpdated = 0;                             // Set flag for no update done
   }
   
   if (storedHeatMode != heatMode) {              // If stored power value is different from working value
    heatModeUpdated = 1;                          // Set flag for update done
    storedHeatMode = heatMode;                    // Update stored mode state to working mode state
    EEPROM.write(setPointAddr+6, storedHeatMode); // Write changes to EEPROM (not done until commit)
   
   } else {
    heatModeUpdated = 0;                           // Set flag for no update done
   }
   
   
   
   if ((powerUpdated == 1) || (setPointUpdated == 1) || heatModeUpdated == 1)  {   // If any setpoint or power state updated

   EEPROM.write(ID_ADDR,EEPROM_ID);                  // write the ID to indicate valid data
   
   EEPROM.commit();                                   // Commit changes to EEPROM

   Serial.println("Value in memory for setpoint");
   Serial.println(EEPROM.read(setPointAddr));
    
   }

 server.send(200, "text/html", EEPROMPage(setPointUpdated, powerUpdated, heatModeUpdated));    // Call EEPROM written webpage
}

void resetPage() {
// CALLS HTML REDIRECT STRING ----------------------------------------------
    server.send(200, "text/html", sendRedirect());  // resets webpage to root
}

void resetSetting() {
// CALLS HTML REDIRECT STRING ----------------------------------------------
    server.send(200, "text/html", settingsRedirect());  // resets webpage to settings
}
 

void handle_NotFound(){     
// HANDLES BAD URL STRING ------------------------------------------------                      
  server.send(404, "text/plain", "Not found");
}

String EEPROMPage(bool setPointUpdated, bool powerUpdated, bool heatModeUpdated) {
// ASSEMBLES HTML STRING FOR EEPROM UPDATE WEBPAGE ------------------------
  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n";
  ptr +="<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}\n";

  ptr +=".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;\n";
  
  ptr +="body{margin-top: 50px;} h1 {color: #444444;margin: 50px auto 30px;}\n";
  ptr +="p {font-size: 24px;color: #444444;margin-bottom: 10px;}\n";
  ptr +="</style>\n";
  ptr +="</head>\n";
  ptr +="<body>\n";
  ptr +="<div id=\"webpage\">\n";

  ptr +="<p><a href=\"/resetPage\"><button class=\"button\">Back</button></a></p>\n";

  
  if (setPointUpdated == 1) {                                   // Displays message as to whether EEPROM updated for setpoint
    ptr +="<p>Updated setpoint in EEPROM.</p>\n";
  } else {
    ptr +="<p>Did not update setpoint, same value in EEPROM.</p>\n";
  }
  if (powerUpdated == 1) {                                      // Displays message as to whether EEPROM updated for power state
    ptr +="<p>Updated power setting in EEPROM.</p>\n";
  } else {
    ptr +="<p>Did not update power setting, same value in EEPROM.</p>\n";
  }
  if (heatModeUpdated == 1) {                                      // Displays message as to whether EEPROM updated for heat state
    ptr +="<p>Updated heat mode setting in EEPROM.</p>\n";
  } else {
    ptr +="<p>Did not update heat mode setting, same value in EEPROM.</p>\n";
  }
  
  ptr +="</html>\n";
  return ptr;                                                   // Return html string to calling argument
  
}


String sendRedirect() { 
// ASSEMBLES HTML FOR WEBPAGE REDIRECT TO MAIN PAGE ---------------------------                          
  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<meta http-equiv=\"Refresh\" content=\"0\; url=/\" />\n";
  ptr +="</html>\n";
  return ptr;
  
}

String settingsRedirect() { 
// ASSEMBLES HTML FOR WEBPAGE REDIRECT TO SETTINGS PAGE ---------------------------                          
  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<meta http-equiv=\"Refresh\" content=\"0\; url=/settings\" />\n";
  ptr +="</html>\n";
  return ptr;
  
}

String settings() {
// ASSEMBLES HTML FOR SETTINGS WEBPAGE -------------------------------------------
  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n";
  ptr +="<title>Settings</title>\n";
  ptr +="<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}\n";

  ptr +=".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;\n";
  
  ptr +="body{margin-top: 50px;} h1 {color: #444444;margin: 50px auto 30px;}\n";
  ptr +="p {font-size: 24px;color: #444444;margin-bottom: 10px;}\n";
  ptr +="</style>\n";
  ptr +="</head>\n";
  ptr +="<body>\n";
  ptr +="<div id=\"webpage\">\n";

  ptr +="<h1>Save Settings</h1>\n";
  ptr +="<p><a href=\"/writeEEPROM\"><button class=\"button\">Save</button></a></p>\n";
  ptr +="<p><a href=\"/eraseEEPROM\"><button class=\"button\">Erase</button></a></p>\n";

  
}
String SendHTML(float Temperaturestat, float Setpoint){   
// ASSEMBLES HTML FOR MAIN WEBPAGE -------------------------------------------
  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n";
  ptr +="<title>Web Enabled Thermostat</title>\n";
  ptr +="<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}\n";

  ptr +=".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;\n";
  
  ptr +="body{margin-top: 50px;} h1 {color: #444444;margin: 50px auto 30px;}\n";
  ptr +="p {font-size: 24px;color: #444444;margin-bottom: 10px;}\n";
  ptr +="</style>\n";
  ptr +="</head>\n";
  ptr +="<body>\n";
  ptr +="<div id=\"webpage\">\n";
  ptr +="<h1>Room Temperature</h1>\n";
  
  ptr +=(float)Temperaturestat;
  ptr +=" F</p>";

  ptr +="<h1>Setpoint</h1>\n";
  ptr +="<p><a href=\"/addDegree\"><button class=\"button\">+</button></a></p>\n";
  ptr +=(float)Setpoint;
  ptr +=" F</p>\n";
  ptr +="<p><a href=\"/minusDegree\"><button class=\"button\">-</button></a></p>\n";

    if(device == 0) {                               // Displays whether device call is on or off
        ptr +="<p>Device is off.</p>\n";
    } else {
        ptr +="<p>Device is on.</p>\n";
    }

  ptr +="<h1>Power</h1>\n";                         // Shows OFF button if power call is off and vice versa
     if (powerSet == 0) {
        ptr +="<p><a href=\"/powerOn\"><button class=\"button\">Off</button></a></p>\n";
     } else {
        ptr +="<p><a href=\"/powerOff\"><button class=\"button\">On</button></a></p>\n";
     }

  
 

  ptr +="<p><a href=\"/settings\"><button class=\"button\">Settings</button></a></p>\n";
  
  ptr +="<h1>Mode</h1>\n";                         // Shows Heat button if heatMode is false
     if (heatMode == 0) {
        ptr +="<p><a href=\"/modeCold\"><button class=\"button\">Heat</button></a></p>\n";
     } else {
        ptr +="<p><a href=\"/modeHeat\"><button class=\"button\">Cool</button></a></p>\n";
     }

  
  ptr +="</div>\n";
  ptr +="</body>\n";
  
  ptr +="</html>\n";
  
  return ptr;                                       // Return string to calling argument
}
