# LifxZoneStrip
Use the Lifx app to control zones with self made LED strips. I'm using APA102 LED Driver strips. (other drivers are also possible)

This library makes use of the APA102 Library written by myself. You can find the library over here: https://github.com/VinzzB/APA102_LedStreamer

This project is a work in progress. 
You can NOT add the LED strip to the Lifx Cloud. You can only control the LED strip from your home network.

All Lifx packets that are necessary to operate normally are included in the library. Some packets are only configured for stability reasons. (eg stopping unnecessary broadcasts = stop phones battery drain)

You can create zones by defening the amount of LEDS for each zone. Keep in mind that the arduino ony has 2Kb of SRAM. You need 9bytes per zone! 1byte = Amount of Leds and 8 bytes for HSBK (colors). if you want more than 255 leds in one or more zones then you need to change the array to an integer type. If you use an integer then 10Bytes are reserved per zone.

This lib also supports The MOVE effect. This is currently the only effect available on Lifx Z / Beam.

Requirements:
- Arduino or Atmega328-P 
- Ws5100 ethernet 
- APA102 LED Strips (you can always hack the code to make it work with other LED strips)
- one 2N3904 Transistor 
- one 10k resistor
----------------------------------------------------------------------------------------
SCHEMATICS
----------------------------------------------------------------------------------------
```
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
```
