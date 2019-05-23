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
             VCC│07  22│GND
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
// - EEPROM Label, group & location (if an EEPROM is available)
// - Waveforms, skew ratio, cycles, periods, transient effects.
// - use SPI.beginTransaction in APA102_LedStreamer. (currently using the clockDivider option.)


#include <SPI.h>
#include <Ethernet.h>
#include <APA102_LedStreamer.h>
#include "lifx.h"
#include "color.h"

//-------------------
//CONFIG 
//-------------------
//Set the amount of leds on the strip 
#define LEDS 300 //0-65535
#define SS_ETHERNET 10
#define RST_ETHERNET 9
#define SS_LED_STRIP 3
#define NETWORK_INTERRUPT_PIN 2 //wire soldered onto ethernet ws5100 LNK led.(optional)
#define DEBUG 1
const uint8_t mac[] = { 0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x02 };
// setup zones
const uint8_t zone_count = 16; //only powers of 8! max 80 zones (by doc). each zone will hold 9bytes in SRam! (1byte for led_zones and 8 bytes for HSBK)
//Define the amount of leds per zone.
//use byte array if every zone has less than 255 leds (=9bytes/zone). Use an unsigned integer array for more (>255) leds per zone (=10bytes per zone).
const uint8_t led_zones[] = { 18, 19, 18, 20, 18, 19, 18, 20, 18 ,19, 18, 20, 18, 19, 18, 20}; 
char bulbLabel[LifxBulbLabelLength] = "Arduino LED Strip";

//-------------------
//GLOBAL VARS
//-------------------
EthernetUDP Udp;
APA102_LedStreamer strip = APA102_LedStreamer(LEDS); 
//allocate memory for zones.
HSBK zones[zone_count];
//MultiZoneEffectPacket effect;
//bool zones_active = false;
uint8_t location[LifxLocOrGroupSize];
uint8_t group[LifxLocOrGroupSize];
uint16_t power_status = 0; //0 | 65535
volatile bool _reInitNetwork = true; //flag set by interrupt when ethernet connection is lost or reconnected.
uint8_t site_mac[] = { 0x4c, 0x49, 0x46, 0x58, 0x56, 0x32 }; // spells out "LIFXV2" - version 2 of the app changes the site address to this...

//keep track of power requests from clients. This will avoid flicker when you press the power button multiple times 
//over a short period of time. Power requests are send multiple times by the Lifx app.
//Not a perfect solution...
uint8_t prev_pwr_seq; //sequence number from last power request.
unsigned long prev_pwr_seq_action = 0; //last time power button was toggled
uint16_t prev_pwr_seq_reset_interval = 5000; //follow client sequence nr for 5sec. accept all requests after that.
//vars for effects. currently only 'move'.
unsigned long last_move_effect_time;
uint32_t move_speed;
uint8_t  move_direction;
uint16_t move_start_led = 0;
 
void setup() {
   //SS for led strip (made with 2N3904 transistor)
  pinMode(SS_LED_STRIP, OUTPUT);
  pinMode(RST_ETHERNET, OUTPUT);
  digitalWrite (RST_ETHERNET, LOW);
  //set up ethernet interrupt (hack for ws5100. Wire soldered on 'LNK' led and connected to digital pin 2).
  pinMode(NETWORK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(NETWORK_INTERRUPT_PIN), reInitNetwork, CHANGE);
    
  SPI.setClockDivider(SPI_CLOCK_DIV2); //8Mhz SPI bus on UNO. Default is 4Mhz (SPI_CLOCK_DIV4) TODO: use SPI.beginTransaction in LedStreamer.
  Serial.begin(115200);
  while (!Serial) { ; /* wait for serial port to connect. Needed for native USB port only*/ }    
  initNetwork();
  // set up a UDP
  Udp.begin(LifxPort);
  //initial light setup.
  setLight();
  //check free ram on device.
  #ifdef DEBUG
    freeRam();
  #endif
}

void loop() {
  //Network changed?
  if(_reInitNetwork) {
    initNetwork();
    return;
  }  
  
  //reset power sequence. This avoids the leds from flashing.  
  if(prev_pwr_seq > 0 && millis() - prev_pwr_seq_action >= prev_pwr_seq_reset_interval) {
    prev_pwr_seq = NULL_BYTE;
  }
  
  //move colors left or right (MOVE Effect for Z strips and Beam)
  if(move_speed > 0 && power_status > 0 && millis() - last_move_effect_time >= (move_speed / LEDS )) {
    last_move_effect_time = millis();

    if(move_direction && move_start_led++ == LEDS - 1)
      move_start_led = 0;
    else if(!move_direction && move_start_led-- == 0)
      move_start_led = LEDS - 1;
    setLight(); //we call the singular functionName because this is a continous stream.  
  }
  
  // push the data into the LifxPacket structure
  LifxPacket request;
  // if there's UDP data available, read a packet. (128 bytes max, defined in lifx.h > LifxPacket.data)
  uint8_t packetSize = Udp.parsePacket();
  if(packetSize > 0 && packetSize <= 128) {
    Udp.read(request.raw, packetSize);
    request.data_size = packetSize - LifxPacketSize; 
    #ifdef DEBUG
      Serial.print("IN ");
      printLifxPacket(request);
    #endif
    //read and respond to the request
     handleRequest(request);    
   }  
  Ethernet.maintain();
}

//Interrupt method (NETWORK_INTERRUPT_PIN)
void reInitNetwork() {
   Serial.println(F("- Network Interrupt!"));
  _reInitNetwork = true;
}

void initNetwork() {
  digitalWrite (SS_LED_STRIP, HIGH);
  digitalWrite (RST_ETHERNET, LOW);
  delay(50);
  digitalWrite (RST_ETHERNET, HIGH);
  // start the Ethernet connection:
  Serial.println(F("Initialize Ethernet with DHCP:"));
  while (Ethernet.begin(mac) == 0) {
    Serial.println(F("Failed to configure Ethernet using DHCP"));
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {     
      Serial.println(F("Ethernet shield was not found.  Sorry, can't run without hardware. :("));
      // no point in carrying on, so do nothing forevermore:
      //while (true) { delay(1); }
      delay(1000);
      continue;
    } else if(Ethernet.linkStatus() == LinkOFF) {
      Serial.println(F("Ethernet cable is not connected."));
    } 
  }
  // print your local IP address:
  Serial.print(F("My IP address: "));
  Serial.println(Ethernet.localIP());
  _reInitNetwork = false;
}

void freeRam () {
  extern int __heap_start, *__brkval;
  int v;
  Serial.print(F("Free RAM: "));
  Serial.println((int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval));  
}

void handleRequest(LifxPacket &request) {  
  uint16_t i = 0; //iterator
  LifxPacket response;
  response.source = request.source;
  response.sequence = request.sequence;

  switch(request.type) {
    case SET_POWER_STATE: {
      if(request.sequence >= prev_pwr_seq +1) {
        prev_pwr_seq = request.sequence;
        prev_pwr_seq_action = millis();      
        power_status = word(request.data[1], request.data[0]);
        setLights();
      }  
    }  
    case GET_POWER_STATE:{
      writeUInt(response.data, 0, power_status);
      createUdpPacket(response, POWER_STATE, 2); 
      break;    
    }
    case SET_LIGHT_STATE: {//102 (0x66)
      for(i = 0; i < zone_count; i++) {
        memcpy(&zones[i], request.data + 1, 8); 
      }
    //  zones_active = false;
      setLights();    
    }
    case GET_LIGHT_STATE: sendLightStateResponse(response); break;   
    
    case SET_WAVEFORM:
    case SET_WAVEFORM_OPTIONAL:{ // 0x77:
      WaveFormPacket dataPacket; //todo: we have other things in this packet! (waveform)
      memcpy(dataPacket.raw, request.data, 25);
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
      sendLightStateResponse(response);
      break;
    }
    case SET_COLOR_ZONES: {
  //    byte apply = request.data[15];  //seems to be buggy?     
      for(i = request.data[0]; i <= request.data[1]; i++) {
        memcpy(&zones[i], request.data + 2,8);         
      }
     // zones_active = true;
      setLights();
      break; 
    }   
    case GET_COLOR_ZONES: {
      for(uint8_t x = 0; x < zone_count; x+=8) {
        response.data[0] = zone_count;
        response.data[1] = x; //first idx nr for each 8 zones send
        for(i = 0; i < 8; i++) { // i = zoneIdx
         memcpy(response.data + 2+(i*8),&zones[x+i],8);
         //if(!zones_active) break;
        }
        //createUdpPacket(response, zones_active ? STATE_MULTI_ZONE : STATE_ZONE, zones_active ? 66 : 10);
        createUdpPacket(response, STATE_MULTI_ZONE, 66 );
        //if(!zones_active) break;
      }
      break; 
    }
    case SET_MULTIZONE_EFFECT: {     
      byte move_enable = request.data[4];
      if(move_enable) {
        move_direction = request.data[31]; //effect.parameters[1];
        memcpy(&move_speed,request.data + 7,4);       
      } else {
        move_speed = 0;
        move_start_led = 0;
      }
    }
    case GET_MULTIZONE_EFFECT:{
      for(i = 0; i < 60; i++) 
        response.data[i] = NULL_BYTE;
      
      response.data[4] = move_speed > 0 ? 1 : 0;
      memcpy(response.data + 7, move_speed, 4);
      response.data[31] = move_direction;
      createUdpPacket(response, STATE_MULTIZONE_EFFECT, 59); 
      break;
    }
    case SET_LOCATION:  memcpy(location, request.data ,LifxLocOrGroupSize);    
    case GET_LOCATION:  createUdpPacket(response, STATE_LOCATION, location, LifxLocOrGroupSize); break;    
    case SET_GROUP:     memcpy(group, request.data ,LifxLocOrGroupSize);    
    case GET_GROUP:     createUdpPacket(response, STATE_GROUP, group, LifxLocOrGroupSize); break;
    case SET_BULB_LABEL:memcpy(bulbLabel, request.data, LifxBulbLabelLength);  
    case GET_BULB_LABEL:createUdpPacket(response, BULB_LABEL, bulbLabel, sizeof(bulbLabel)); break;
    case GET_MESH_FIRMWARE_STATE:     
    case GET_WIFI_FIRMWARE_STATE: {//0x12 
      createUdpPacket(response, 
                      request.type == GET_WIFI_FIRMWARE_STATE ? WIFI_FIRMWARE_STATE : MESH_FIRMWARE_STATE, 
                      FirmwareVersionData, 20);
      break;  
    }        
    case GET_VERSION_STATE: {
      // respond to get command
      writeUInt(response.data,0,LifxBulbVendor);  writeUInt(response.data,2,NULL_BYTE);
      writeUInt(response.data,4,LifxBulbProduct); writeUInt(response.data,6,NULL_BYTE);
      writeUInt(response.data,8,LifxBulbVersion); writeUInt(response.data,10,NULL_BYTE);
      createUdpPacket(response, VERSION_STATE, 12);
      break;
    }
    case GET_PAN_GATEWAY: {
      response.data[0] = SERVICE_UDP;
      writeUInt(response.data,1,LifxPort);
      writeUInt(response.data,3,NULL_BYTE);
      createUdpPacket(response, PAN_GATEWAY, 5);    
      break;
    }
    case GET_WIFI_INFO:{
      //write float signal (4bytes)
      writeUInt(response.data,0,0x3F7D); //0.99
      writeUInt(response.data,2,0x70A4); //0.99
      for(i=4; i < 12;i++)
        response.data[i] = NULL_BYTE;      
      //write short mcu_temp (2bytes)
      writeUInt(response.data,12,20);    
      createUdpPacket(response, STATE_WIFI_INFO, 14);
      break;   
    }
    case 54: {//0x36
      //Unknown packet type. It has something to do with the Lifx cloud.Client asks bulb if it is attached to the cloud or not?
      //Responding with packet type 56 and 16 bytes as data stops the broadcast madness...
      for(i=0;i<16;i++)
        response.data[i] = NULL_BYTE;      
      createUdpPacket(response, 56, 16);
      break;
    }
    case 701: {
      //Responding with an empty 702 packet keeps the client happy.
      createUdpPacket(response, 702, 0);
      break;
    } 
/*************
  DEBUG
**************/      

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
  }
}

void sendLightStateResponse(LifxPacket &response) {
  memcpy(response.data,zones[0].raw,8);
  writeUInt(response.data,8,NULL_BYTE);
  writeUInt(response.data,10,power_status);
  uint8_t i = 0;
  // label
  for(i = 0; i < 32; i++) {
    response.data[12+i] = lowByte(bulbLabel[i]);
  }
  //tags (reserved)
  for(i = 0; i < 8; i++) {
    response.data[44+i] = NULL_BYTE;
  }  
  createUdpPacket(response, LIGHT_STATUS, 52);
}

void writeUInt(uint8_t data[], uint8_t offset, uint16_t value) {
  data[offset] = value;
  data[offset+1] = value >> 8;
}

void createUdpPacket(LifxPacket &response, uint16_t packet_type, uint8_t data[], uint8_t data_size) {      
  memcpy(response.data, data, data_size); //todo, is this necessary???
  createUdpPacket(response, packet_type, data_size);
}

void createUdpPacket(LifxPacket &response, uint16_t packet_type, uint8_t data_size) {
  response.res_required = response.ack_required = false;         
  response.type =  packet_type;
  response.protocol = 1024;
  response.reserved2 = NULL_BYTE;
  response.addressable = true;  
  memcpy(response.target, mac, 6);
  response.target[7] = response.target[6] = NULL_BYTE;
  memcpy(response.reserved1, site_mac, 6); 
  response.data_size = data_size;
  response.size = response.data_size + LifxPacketSize;
  sendUDPPacket(response);    
}

void sendUDPPacket(LifxPacket &pkt) { 
  // broadcast packet on local subnet
  IPAddress remote_addr(Udp.remoteIP());
  IPAddress broadcast_addr(remote_addr[0], remote_addr[1], remote_addr[2], 255); //assumes a /24 network. (todo, get subnet from dhcp.)

  #ifdef DEBUG
    Serial.print(F("OUT "));
    printLifxPacket(pkt);
    Serial.println();
  #endif

  Udp.beginPacket(broadcast_addr, Udp.remotePort());
  for(uint8_t x = 0; x < pkt.size; x++) Udp.write(pkt.raw[x]);
  Udp.endPacket();
}

void setLights() {
  //called twice. This forces the internal LED buffer to show the values onto the LED. (only for LED Strips with APA102 drivers)
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
      strip.setNextLeds(rgb[0],rgb[1],rgb[2], constrain(color.bri,0,31), leds_count);
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

void printLifxPacket(LifxPacket &request) {
    uint8_t i = 0;
    Serial.print(F("LFX2: Size:"));
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
    for(i = 0; i < request.data_size; i++) {
      Serial.print(request.data[i], HEX);
      Serial.print(F(" "));
    }    
        
//    Serial.print(F(" | data_size:"));
//    Serial.print(request.data_size);
    Serial.println();  
}
