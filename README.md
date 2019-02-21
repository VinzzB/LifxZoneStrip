# LifxZoneStrip
Use the Lifx app to control zones with self made LED strips. I'm using APA102 LED Driver strips. (other drivers are also possible)

This library makes use of the APA102 Library written by myself. You can find the library over here: https://github.com/VinzzB/APA102_LedStreamer

This project is a work in progress. 
You can NOT add the LED strip to the Lifx Cloud. You can only control the LED strip from your home network.

All Lifx packets that are necessary to operate normally are included in the library. Some packets are only configured for stability reasons. (eg stopping unnecessary broadcasts = stop phones battery drain)

You can crate your own zones by defening the amount of LEDS for each zone. Keep in mind that the arduino ony has 2Kb of SRAM. You need 9bytes per zone! 1byte = Amount of Leds and 8 bytes for HSBK (colors).

This lib also supports The MOVE effect.
