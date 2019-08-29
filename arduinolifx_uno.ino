/*--------------------------------------------------------------
//Created by VinzzB (https://github.com/VinzzB/LifxZoneStrip)
//--------------------------------------------------------------

Requirements:
- Arduino or Atmega328-P (or variants)
- Ws5100 ethernet (or variant)
- APA102 LED Strips (you can always hack the code to make it work with other LED strips)
- one 2N3904 Transistor (or variants)
- one 10k resistor

----------------------------------------------------------------------------------------
SCHEMATICS (Copy/paste to notepad if lines not match)
----------------------------------------------------------------------------------------

                ┌──────┐
             RST│01  28│A5
              D0│02  27│A4
              D1│03  26│A3
  ETH_INT ─── D2│04  25│A2
              D3│05  24│A1
              D4│06  23│A0
             VCC│07  22│GND  ──── GND_LED  
             GND│08  21│aref
            XTAL│09  20│VCC
            XTAL│10  19│D13 ── SCK ────> ETHERNET ────────┐
              D5│11  18│D12 ── MISO ───> ETHERNET         │
          ┌── D6│12  17│D11 ── MOSI ───> ETHERNET / LED   │
          │   D7│13  16│D10 ── SS_ETH ─> ETHERNET         │
          │   D8│14  15│D9                 ┌──────────────┘
          │     └──────┘                   E
          └────────10k─────────SS_LED──> B ► (2N3904 NPN)
                                           C
                                           │
                                       SCK_LED

ETH_INT = Ethernet interrupt (optional)

*/
// TODO: 
// - Waveforms, skew ratio, cycles, periods, transient effects. (see: https://github.com/tigoe/ArduinoLifx)
// - use SPI.beginTransaction in APA102_LedStreamer. (currently using the clockDivider option.)

/*-------------------
//LOAD OR INCLUDE FUNCTIONALITY
//------------------- */
#define USE_NETWORK_INTERRUPT_HACK //uncomment this line if have a ws5100 network chip with a wire hack.
#define USE_EEPROM //uncomment this line if you are using an arduino with EEPROM support and you want to save the Label, location and group in non volatile memory.
#define DEBUG

#include <SPI.h>
#include <Ethernet.h>
#include <APA102_LedStreamer.h>
#include "lifx.h"
#include "color.h"
#ifdef USE_EEPROM
  #include <EEPROM.h>
#endif

/*-------------------
// CONFIG  (replace settings if needed)
//------------------- */

//Set the amount of leds on the strip 
#define LEDS 300 //0-65535
#define LABEL "Arduino LED Strip"
#define SS_ETHERNET 10
#define RST_ETHERNET 9
#define SS_LED_STRIP 3
#define NETWORK_INTERRUPT_PIN 2 //wire soldered onto ethernet ws5100 LNK led.(optional)

const uint8_t mac[] = { 0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x02 };
// setup zones
const uint8_t zone_count = 16; //only powers of 8! max 80 zones (by doc). each zone will hold 9bytes in SRam! (1byte for led_zones and 8 bytes for HSBK)
//Define the amount of leds per zone.
//use byte array if every zone has less than 255 leds (=9bytes/zone). Use an unsigned integer array for more (>255) leds per zone (=10bytes per zone).
const uint8_t led_zones[] = { 18, 19, 18, 20, 18, 19, 18, 20, 18 ,19, 18, 20, 18, 19, 18, 20}; 

/*-------------------
// GLOBAL VARS  (do not change, unless you know what you're doing!)
//------------------- */
EthernetUDP Udp;
APA102_LedStreamer strip = APA102_LedStreamer(LEDS); 
//allocate memory for zones.
HSBK zones[zone_count];
uint16_t power_status = 0; //0 | 65535
#ifdef USE_NETWORK_INTERRUPT_HACK
  volatile bool _reInitNetwork = true; //flag set by interrupt when ethernet connection is lost or reconnected.
#endif
uint8_t site_mac[] = { 0x4c, 0x49, 0x46, 0x58, 0x56, 0x32 }; // spells out "LIFXV2" - version 2 of the app changes the site address to this...

//vars for Lifx-Z effects. currently only 'move'.
unsigned long last_move_effect_time;
uint32_t move_speed;
uint8_t  move_direction;
uint16_t move_start_led = 0;
IPAddress broadcastIp; //Automatically calculated from IP and subnet:

union lifxEeprom {
  uint8_t raw[136];
  struct {
    char label[LifxBulbLabelLength] = LABEL;  //32 bytes
    uint8_t location[LifxLocOrGroupSize]; // = guid + label //48 bytes
    uint8_t group[LifxLocOrGroupSize];    // = guid + label //48 bytes
    uint64_t grp_loc_updated_at;                            //8  bytes
  };
};

lifxEeprom eeprom; 
 
void setup() {
   //SS for led strip (made with 2N3904 transistor)
  pinMode(SS_LED_STRIP, OUTPUT);
  pinMode(RST_ETHERNET, OUTPUT);
  digitalWrite(RST_ETHERNET, LOW); //keep low on startup until networik is properly loaded in initNetwork()
  
  //set up ethernet interrupt (hack for ws5100. Wire soldered on 'LNK' led and connected to digital pin 2).
  #ifdef USE_NETWORK_INTERRUPT_HACK
    pinMode(NETWORK_INTERRUPT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(NETWORK_INTERRUPT_PIN), reInitNetwork, CHANGE);
  #endif
    
  SPI.setClockDivider(SPI_CLOCK_DIV2); //8Mhz SPI bus on UNO. Default is 4Mhz (SPI_CLOCK_DIV4) TODO: use SPI.beginTransaction in LedStreamer.
  Serial.begin(115200);
  while (!Serial) { ; /* wait for serial port to connect. Needed for native USB port only*/ }    
   //read settings from eeprom (if available)
  #ifdef USE_EEPROM
    readEEPROM();
  #endif
  //initialize network
  initNetwork();
  //check free ram on device. (debug)
  #ifdef DEBUG
    freeRam();
  #endif
}

void loop() {
  //Network changed?
  #ifdef USE_NETWORK_INTERRUPT_HACK
    if(_reInitNetwork)
      initNetwork();
  #endif    

  //--------------------
  // Animations: move colors left or right (MOVE Effect for Z strips and Beam)
  //--------------------
  if(move_speed > 0 && power_status > 0 && millis() - last_move_effect_time >= (move_speed / LEDS )) {
    last_move_effect_time = millis();

    if(move_direction && move_start_led++ == LEDS - 1)
      move_start_led = 0;
    else if(!move_direction && move_start_led-- == 0)
      move_start_led = LEDS - 1;
    setLight(); //we call the singular functionName because this is a continous stream.  
  }
  
  //--------------------
  //Handle network data.
  //--------------------
  LifxPacket request;
  // if there's UDP data available, read a packet.
  uint16_t packetSize = Udp.parsePacket();
  if(packetSize >= LifxPacketSize) {
    //read first 36 bytes. 
    Udp.read(request.raw, LifxPacketSize);    
    request.data_size = packetSize - LifxPacketSize;     
    handleRequest(request);
  }
  Ethernet.maintain();
}

//Interrupt method (NETWORK_INTERRUPT_PIN)
#ifdef USE_NETWORK_INTERRUPT_HACK
  void reInitNetwork() {
     Serial.println(F("- Network Interrupt!"));
    _reInitNetwork = true;
  }
#endif

void initNetwork() {
  digitalWrite (RST_ETHERNET, LOW);
  delay(50);
  digitalWrite (RST_ETHERNET, HIGH);
  // start the Ethernet connection:
  Serial.println(F("Initialize Ethernet with DHCP:"));
  while (Ethernet.begin(mac) == 0) {
    Serial.println(F("Failed to configure Ethernet using DHCP"));
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {     
      Serial.println(F("Ethernet shield was not found.  Sorry, can't run without hardware. :("));
    } else if(Ethernet.linkStatus() == LinkOFF) {
      Serial.println(F("Ethernet cable is not connected."));
    } 
     delay(1000);
  }  
  /*
  //Without DHCP: comment the while loop above and uncomment the following lines :  
  IPAddress ip(192, 168, 0, 254); 
  Ethernet.begin(mac, ip); //asumes a /24 subnet (see doc for more options)
  */
  
  // Calculate broadcast address from IP and subnet.
  // broadcastIp = IPAddress((Ethernet.localIP() & Ethernet.subnetMask()) | ~Ethernet.subnetMask());
  Serial.print(F("My IP address: "));
  Serial.println(Ethernet.localIP());    
//  Serial.print(F("Broadcast address: "));
//  Serial.println(broadcastIp);
  // set up UDP Port (56700)
  Udp.begin(LifxPort);   
  // blink 3x Green on DHCP success
  digitalWrite (SS_LED_STRIP, true);
  for(byte x = 0; x < 6;x++) {
    strip.setLeds(0, x % 2 ? 255 : 0, 0, 10, false);      
    delay(200);
  }    
  setLight(); //(back to previous) light setup.
  //reset interrupt flag (if available).
  #ifdef USE_NETWORK_INTERRUPT_HACK
    _reInitNetwork = false;
  #endif  
}

#ifdef USE_EEPROM
  void printLocOrGroup(uint8_t data[]){
    for(byte x = 16; x < LifxLocOrGroupSize;x++)
      Serial.write(data[x]);  
  }

  char eeprom_check[] = { 'L','I','F','X' };
  void readEEPROM() {
      Serial.println(F("Restoring bulb settings from EEPROM..:"));
      //read 136 bytes from eeprom and push into eeprom struct. (offset 4)     
      for(byte x = 0; x < 140; x++) {
        byte b = EEPROM.read(x);      
        if(x > 3) //push every byte beyond position 3 in struct
          eeprom.raw[x-4] = b;
        else if(b != eeprom_check[x]) { //the first 4 bytes must spell LIFX
          Serial.println(F("EEPROM does not contain LIFX settings."));
          break; //if not, exit the loop.
        }
      }
    
    #ifdef DEBUG
      Serial.print(F("Label: "));
      Serial.println(eeprom.label);
      Serial.print(F("Location: "));
      printLocOrGroup(eeprom.location);
      Serial.println();      
      Serial.print(F("Group: "));
      printLocOrGroup(eeprom.group);
      Serial.println();
    #endif
  }

  void writeEEPROM() {
     Serial.print(F("Writing settings to EEPROM..."));
     for(byte x = 0; x < 4; x++)
        EEPROM.update(x, eeprom_check[x]);
     for(byte x = 4; x < 140; x++) {
        EEPROM.update(x, eeprom.raw[x-4]);
     }
     Serial.println(F("Done!"));
  }
#endif

void freeRam () {
  extern int __heap_start, *__brkval;
  int v;
  Serial.print(F("Free RAM: "));
  Serial.println((int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval));  
}

void handleRequest(LifxPacket &request) {  
  uint16_t i = 0; //global iterator used in this method for various stuff.  
  LifxPacket response;  //first 36 bytes; 
  response.source = request.source;
  response.sequence = request.sequence;
  //get lifx payload.
  uint8_t reqData[ request.type == SET_EXTENDED_COLOR_ZONES ? 8 : request.data_size ];          
  Udp.read(reqData, request.type == SET_EXTENDED_COLOR_ZONES ? 8 : request.data_size);
  byte responseData[LifxMaximumPacketSize-LifxPacketSize]; //Lifx Payload for response
  #ifdef DEBUG
    Serial.print("IN ");
    printLifxPacket(request, reqData);
  #endif  

  switch(request.type) {
    case /* 0x75 (117) */ SET_POWER_STATE: {     
        power_status = word(reqData[1], reqData[0]); // 0 | 65535
        setLights();            
    }  
    case /* 0x74 (116) */ GET_POWER_STATE:{   
      writeUInt(responseData, 0, power_status);
      return prepareAndSendPacket(request, response, POWER_STATE,responseData, 2); //0x76 (118)   
    }
    case /* 0x66 (102) */ SET_LIGHT_STATE: {
      for(i = 0; i < zone_count; i++) {
        memcpy(&zones[i], reqData + 1, 8); 
      }
      setLights();    
    }
    case /* 0x65 (101) */ GET_LIGHT_STATE: sendLightStateResponse(request, response, responseData); break;
    
    case /* 0x67 (103) */ SET_WAVEFORM:
    case /* 0x77 (119) */ SET_WAVEFORM_OPTIONAL:{ 
      WaveFormPacket dataPacket; //todo: we have other things in this packet! (waveform)
      memcpy(dataPacket.raw, reqData, 25);
      for(i = 0; i < zone_count; i++) {
        if(dataPacket.set_hue || request.type == SET_WAVEFORM)
          zones[i].hue = dataPacket.color.hue;
        if(dataPacket.set_saturation || request.type == SET_WAVEFORM)
          zones[i].sat = dataPacket.color.sat; 
        if((dataPacket.set_brightness && zones[i].bri > 0) || request.type == SET_WAVEFORM)
          zones[i].bri = dataPacket.color.bri;                                                                                                                     
        if(dataPacket.set_kelvin || request.type == SET_WAVEFORM)
          zones[i].kel = dataPacket.color.kel; 
      }
      setLights();       
      return sendLightStateResponse(request, response, responseData);
    }
    case /* 0x1F5 (501) */ SET_COLOR_ZONES: {
  //    byte apply = request.data[15];  //seems to be buggy?     
      for(i = reqData[0]; i <= reqData[1]; i++) {
        memcpy(&zones[i], reqData + 2,8);         
      }
      setLights();
      if(request.ack_required)
        return prepareAndSendPacket(request, response, LIFX_ACK, responseData, 0);      
      if(!request.res_required)
        break;
    }   
    case /* 0x1F6 (502) */ GET_COLOR_ZONES: {
      for(uint8_t x = 0; x < zone_count; x+=8) {
        responseData[0] = zone_count;
        responseData[1] = x; //first idx nr for each 8 zones send
        for(i = 0; i < 8; i++) { // i = zoneIdx
          memcpy(responseData + 2+(i*8),&zones[x+i],8);
        }
        prepareAndSendPacket(request, response, STATE_MULTI_ZONE, responseData, 66 ); //506
      }
      break; 
    }
    case /* 0x1FC (508) */ SET_MULTIZONE_EFFECT: {
      byte move_enable = reqData[4];
      if(move_enable) {
        move_direction = reqData[31]; //effect.parameters[1];
        memcpy(&move_speed,reqData + 7,4);       
      } else {
        move_speed = 0;
        move_start_led = 0;
      }
    }
    case /* 0x1FB (507) */ GET_MULTIZONE_EFFECT:{
      for(i = 0; i < 60; i++) 
        responseData[i] = NULL_BYTE;
      
      responseData[4] = move_speed > 0 ? 1 : 0;
      memcpy(responseData + 7, move_speed, 4);
      responseData[31] = move_direction;
      return prepareAndSendPacket(request, response, STATE_MULTIZONE_EFFECT, responseData, 59); //0x1FD (509)
    }
    case /* 0x31 (49) */ SET_LOCATION: {
      memcpy(eeprom.location, reqData ,LifxLocOrGroupSize);
      memcpy(&eeprom.grp_loc_updated_at, reqData + 48, 8);
      #ifdef USE_EEPROM
        writeEEPROM();
      #endif
    }
    case /* 0x30 (48) */ GET_LOCATION: {
      memcpy(responseData, eeprom.location, LifxLocOrGroupSize);
      memcpy(responseData + 48, &eeprom.grp_loc_updated_at, 8);
      return prepareAndSendPacket(request, response, STATE_LOCATION, responseData, 56); //0x32 (50)   
    }
    case /* 0x33 (51) */ SET_GROUP: {  
      memcpy(eeprom.group, reqData ,LifxLocOrGroupSize);
      memcpy(&eeprom.grp_loc_updated_at, reqData + 48, 8);
      #ifdef USE_EEPROM
        writeEEPROM();
      #endif
    }
    case /* 0x34 (52) */ GET_GROUP: {
      memcpy(responseData, eeprom.group, LifxLocOrGroupSize);      
      memcpy(responseData + 48, &eeprom.grp_loc_updated_at, 8);      
      return prepareAndSendPacket(request, response, STATE_GROUP, responseData, 56); //0x35 (53)
    }    
    case /* 0x18 (24) */ SET_BULB_LABEL: {
      memcpy(&eeprom.label, &reqData, LifxBulbLabelLength);
      #ifdef USE_EEPROM
        writeEEPROM();
      #endif
    }
    case /* 0x17 (23) */ GET_BULB_LABEL:
      return prepareAndSendPacket(request, response, BULB_LABEL, eeprom.label, sizeof(eeprom.label));  //0x19 RSP 25
    case /* 0x0e (14) */ GET_MESH_FIRMWARE_STATE:  
    case /* 0x12 (18) */ GET_WIFI_FIRMWARE_STATE: {
      return prepareAndSendPacket(request, response, 
          request.type == GET_WIFI_FIRMWARE_STATE ? WIFI_FIRMWARE_STATE : MESH_FIRMWARE_STATE, 
          FirmwareVersionData, 20);  
    }        
    case /* 0x20 (32) */ GET_VERSION_STATE: {
      // respond to get command
      writeUInt(responseData,0,LifxBulbVendor);  writeUInt(responseData,2,NULL_BYTE);
      writeUInt(responseData,4,LifxBulbProduct); writeUInt(responseData,6,NULL_BYTE);
      writeUInt(responseData,8,LifxBulbVersion); writeUInt(responseData,10,NULL_BYTE);
      return prepareAndSendPacket(request, response, VERSION_STATE, responseData, 12); //0x21  (33)
    }
    case /* 0x02 (2) */ GET_PAN_GATEWAY: { 
      responseData[0] = SERVICE_UDP;
      writeUInt(responseData,1,LifxPort);
      writeUInt(responseData,3,NULL_BYTE);
      return prepareAndSendPacket(request, response, PAN_GATEWAY, responseData, 5);   //0x03 (3)
    }
    case /* 0x10 (16) */ GET_WIFI_INFO:{
      //write float signal (4bytes)
      writeUInt(responseData,0,0x3F7D); //0.99
      writeUInt(responseData,2,0x70A4); //0.99
      for(i=4; i < 12;i++)
        responseData[i] = NULL_BYTE;      
      //write short mcu_temp (2bytes)
      writeUInt(responseData,12,20);    
      return prepareAndSendPacket(request, response, STATE_WIFI_INFO, responseData, 14); // 0x11 (17)
    }
    case /* 510 */ SET_EXTENDED_COLOR_ZONES: {
      uint16_t index = word(reqData[6], reqData[5]);
      //get each HSBK value as stream and put it in the appropriate zone.
      for(i = 0;i< reqData[7] ;i++)
        Udp.read(zones[index + i].raw, 8);          
      setLights(); 
      if(request.ack_required)
        return prepareAndSendPacket(request, response, LIFX_ACK, responseData, 0);      
      if(!request.res_required)
        break;      
    }
    case /* 511 */ GET_EXTENDED_COLOR_ZONES: {
      prepareResponse(response, STATE_EXTENDED_COLOR_ZONES, 5 + (8 * zone_count));
      writeUInt(responseData,0,zone_count);
      writeUInt(responseData,2,0); //index
      responseData[4] = zone_count; //colors_count
      sendPacket(response, responseData, 5 , false);
      //stream each hsbk zone value to client.
      for(i = 0; i < zone_count; i++)
        Udp.write(zones[i].raw, 8);
      Udp.endPacket();
      break;
    }    
    case /* 0x36  */ 54: {
      //Unknown packet type. It has something to do with the Lifx cloud.Client asks bulb if it is attached to the cloud or not?
      //Responding with packet type 56 and 16 bytes as data stops the broadcast madness...
      for(i=0;i<16;i++)
        responseData[i] = NULL_BYTE;      
      return prepareAndSendPacket(request, response, 56, responseData, 16);
    }
    case 701: {
      //Responding with an empty 702 packet keeps the client happy.
      return prepareAndSendPacket(request, response, 702, responseData, 0);
    } 
/*************
  DEBUG
**************/      
    #ifdef DEBUG
    default: {
      if(request.target[0] == mac[0] || request.target[0] == 0) {
        Serial.print(F("-> Unknown packet type, ignoring 0x"));
        Serial.print(request.type, HEX);
        Serial.print(" (");
        Serial.print(request.type);
        Serial.println(")");
      }
      break;
    }
    #endif
  }
}

void sendLightStateResponse(LifxPacket &request, LifxPacket &response, uint8_t responseData[]) {
  memcpy(responseData,zones[0].raw,8);
  writeUInt(responseData,8,NULL_BYTE);
  writeUInt(responseData,10,power_status);
  uint8_t i = 0;
  // label
  for(i = 0; i < 32; i++) {
    responseData[12+i] = lowByte(eeprom.label[i]);
  }
  //tags (reserved)
  for(i = 0; i < 8; i++) {
   responseData[44+i] = NULL_BYTE;
  }  
  prepareAndSendPacket(request, response, LIGHT_STATUS, responseData, 52);
}

void writeUInt(uint8_t data[], uint8_t offset, uint16_t value) {
  data[offset] = value;
  data[offset+1] = value >> 8;
}

void prepareResponse(LifxPacket &response, uint16_t packet_type, uint16_t data_size) {
  response.res_required = response.ack_required = false;         
  response.type =  packet_type;
  response.protocol = 1024;
  response.reserved2 = NULL_BYTE;
  response.addressable = true;  
  memcpy(response.target, mac, 6);
  response.target[7] = response.target[6] = NULL_BYTE;
  memcpy(response.reserved1, site_mac, 6); 
  response.data_size = data_size; //NOT included in response
  response.size = response.data_size + LifxPacketSize; //included in response!
}
void prepareAndSendPacket(LifxPacket &request, LifxPacket &response, uint16_t packet_type,uint8_t responseData[], uint16_t data_size) {  
  prepareResponse(response,
    request.ack_required ? LIFX_ACK : packet_type,
    request.ack_required ?  0 : data_size);  
  sendPacket(response, responseData);
}

void sendPacket(LifxPacket &pkt, uint8_t responseData[]) {   
  sendPacket(pkt, responseData, pkt.data_size, true);
}

void sendPacket(LifxPacket &pkt, uint8_t responseData[], uint16_t data_size, bool finishPacket) {   
  #ifdef DEBUG
    Serial.print(F("OUT "));       
    printLifxPacket(pkt, responseData);
  #endif
  Udp.beginPacket(Udp.remoteIP(), Udp.remotePort()); //startUdpSendPacket():
 // Udp.beginPacket(broadcastIp, LifxPort); //BROADCASTS (test sync to multiple clients)
  Udp.write(pkt.raw, LifxPacketSize); 
  Udp.write(responseData, data_size);
  if(finishPacket)
    Udp.endPacket();
}

void setLights() {
  //called twice. This forces the internal LED buffer to show the values onto the LED strip. (only for LED Strips with APA102 drivers)
  //You should only call it once if you have a continuous stream of changing leds.
  setLight(); setLight(); 
}

void setLight() {
  selectSpiLedStrip(true);
  if(power_status) {
    byte currentZoneIdx = 0;
    uint16_t startZoneLed = led_zones[0];
    for(startZoneLed; startZoneLed < move_start_led; startZoneLed += led_zones[++currentZoneIdx]) {/*EMPTY*/}
    startZoneLed -=  move_start_led;
    for(uint8_t zoneIdx = currentZoneIdx; zoneIdx < zone_count; zoneIdx++) {
      writeToStrip(zones[zoneIdx], zoneIdx == currentZoneIdx ? startZoneLed : led_zones[zoneIdx]);
    }
    for(uint8_t zoneIdx = 0; zoneIdx <= currentZoneIdx; zoneIdx++) {
      writeToStrip(zones[zoneIdx], led_zones[zoneIdx] - (zoneIdx == currentZoneIdx ? startZoneLed : 0));
    }
  } else {   
    strip.setLeds(0,0,0,0,false);   
  }
  selectSpiLedStrip(false);
}

void selectSpiLedStrip(byte enable) {
  digitalWrite (SS_ETHERNET, !enable);
  digitalWrite (SS_LED_STRIP, enable);
}

void writeToStrip(HSBK color, uint16_t leds_count) {
  uint16_t this_hue = map(color.hue, 0, 65535, 0, 360);
  uint8_t this_sat = map(color.sat, 0, 65535, 0, 255);
  uint8_t this_bri = map(color.bri, 0, 65535, 0, 255);
  // if we are setting a "white" colour (kelvin temp)
  if(color.kel > 0 && this_sat < 1) {
    // convert kelvin to RGB
    rgb kelvin_rgb = kelvinToRGB(color.kel);
    // convert the RGB into HSV
    hsv kelvin_hsv = rgb2hsv(kelvin_rgb);
    // set the new values ready to go to the bulb (brightness does not change, just hue and saturation)
    this_hue = kelvin_hsv.h;
    this_sat = map(kelvin_hsv.s*1000, 0, 1000, 0, 255); //multiply the sat by 1000 so we can map the percentage value returned by rgb2hsv
  }
  uint8_t rgb[] ={0,0,0};
  hsb2rgb(this_hue, this_sat, this_bri, rgb);     
  strip.setNextLeds(rgb[0],rgb[1],rgb[2], constrain(color.bri,0,31), leds_count); //my APA102 has 32 levels of brightness.
}

/*
Convert a HSB color to RGB
This function is used internally but may be used by the end user too. (public).
@param h The hue (0..65535) (Will be % 360)
@param s The saturation value (0..255)
@param b The brightness (0..255)
*/
void hsb2rgb(uint16_t hue, uint8_t sat, uint8_t val, uint8_t rgb[]) {
  uint8_t setColorIdx[] {1,0,2};
  hue = hue % 360;  
  rgb[0] = rgb[1] = rgb[2] = val; // (only if sat == 0) Acromatic color (gray). Hue doesn't mind.
  if (sat != 0) {      
    uint8_t base = ((255 - sat) * val)>>8;
    uint8_t slice = hue / 60;
    rgb[ slice < 2 ? 2 : slice < 4 ? 0 : 1 ] = base;    
    rgb[ setColorIdx[slice % 3] ] = (((val-base)*(!slice?hue:(slice%2?(60-(hue%60)):(hue%60))))/60)+base;
  }
}

void printLifxPacket(LifxPacket &request, uint8_t reqData[]) {
    uint8_t i = 0;

    Serial.print(Udp.remoteIP()); //todo: broadcast or unicast...
    Serial.print(F(":"));    
    Serial.print(Udp.remotePort()); //todo: will change when implementing decent broadcasts
    
    Serial.print(F(" Size:"));
    Serial.print(request.size);
    
//    Serial.print(F(" | Proto: "));
//    Serial.print(request.protocol);

//    Serial.print(F(" (0x"));
//    Serial.print(request.raw[2], HEX);
//    Serial.print(F(" "));
//    Serial.print(request.raw[3], HEX);
//    Serial.print(F(" "));
    //Serial.print(F(")"));
    
//    Serial.print(F(") | addressable: "));
//    Serial.print(request.addressable);
//    
//    Serial.print(F(" | tagged: "));
//    Serial.print(request.tagged);
//
//    Serial.print(F(" | origin: "));
//    Serial.print(request.origin);

    Serial.print(F(" | source: 0x"));
    Serial.print(request.source, HEX);

    Serial.print(F(" | target: 0x"));
    for(i = 0; i < 8; i++) {
      Serial.print(request.target[i], HEX);
      Serial.print(F(" "));
    }
    
//    Serial.print(F(" | reserved1: 0x"));
//    for(i = 0; i < 6; i++) {
//      Serial.print(request.reserved1[i], HEX);
//      Serial.print(F(" "));
//    }
    
    Serial.print(F(" | res_required:"));
    Serial.print(request.res_required);

    Serial.print(F(" | ack_required:"));
    Serial.print(request.ack_required);

//    Serial.print(F(" | reserved2: 0x"));
//    Serial.print(request.reserved2, HEX);

    Serial.print(F(" | sequence: 0x"));
    Serial.print(request.sequence, HEX);
    
//    Serial.print(F(" | reserved3: 0x"));
//    Serial.print(request2.reserved3, HEX);
    
    Serial.print(F(" | type: 0x"));
    Serial.print(request.type, HEX); 
    Serial.print(" (");
    Serial.print(request.type);
    Serial.print(")"); 
     
//    Serial.print(F(" | reserved4: 0x"));
//    Serial.print(request.reserved4, HEX);

    Serial.print(F(" | data: "));
    if(request.type == 510 || request.type == 512)
      Serial.print(F("< Stream >"));
    else {
      for(i = 0; i < request.data_size; i++) {
        Serial.print(reqData[i], HEX);
        Serial.print(F(" "));
      }    
    }
        
//    Serial.print(F(" | data_size:"));
//    Serial.print(request.data_size);
    Serial.println();  
}
