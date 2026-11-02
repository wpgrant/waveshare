# Badge for the Waveshare ESP32-S3-Touch-AMOLED-1.75C

A standalone badge sketch for the same 466×466 round AMOLED board used by
`booth-display-175c`. It starts on a sample landscape illustration and uses
the side BOOT button to cycle through:

1. Sample picture placeholder
2. **Got evals?**
3. LinkedIn QR code
4. Battery percentage

The QR code encodes the LinkedIn URL configured near the top of
`badge.ino`. Change `LINKEDIN_URL` if you want to use a different profile.

## Button and power

- Short-press BOOT to advance to the next screen. The order wraps around.
- The battery screen reads the onboard AXP2101 power manager and refreshes
  about every 10 seconds.
- Hold BOOT for about 1.2 seconds, then release when prompted, to turn off
  the display and enter deep sleep. Press BOOT to wake; the sketch restarts
  on the picture screen.
- This is software deep sleep, not a complete hardware power-off. For a full
  power-off, use the board's PWR button as usual. Do not hold BOOT while
  powering on: that can put the ESP32 into download mode.

## Arduino IDE setup

1. Install ESP32 board support, **GFX Library for Arduino**
   (moononournation). No separate battery library is required; the sketch
   reads the onboard AXP2101 over I²C.
2. Open `badge.ino` from this folder.
3. Select **ESP32S3 Dev Module** with **USB CDC On Boot: Enable**,
   **Flash Size: 32MB**, and **PSRAM: OPI PSRAM**.
4. Upload to the board.

The board pin map and Nayuki QR-code generator are included in this folder,
so no touch, Wi-Fi, or battery-library installation is needed.
