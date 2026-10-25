# Booth Display v2 for the Waveshare ESP32-S3-Touch-AMOLED-1.75C

A menu-driven booth app for the 1.75" round AMOLED (466×466). From the menu:

| Menu item | What it does |
|---|---|
| **Flipbook** | Photos (if you ran `convert_photos.py`) or an intro slide, auto-advancing every 10 s in kiosk mode. |
| **QR Codes** | Swipe through three QR codes: **LinkedIn**, **vCard contact card**, **Book a meeting**. Flip to the right one for who you're talking to. |
| **Badge** | Wearable badge mode. Tap to switch between an animated **"Got evals?"** screen and a photo version (your first photo, or a static "Got evals?" slide if you have no photos). |
| **Clicker** | Bluetooth presentation remote — pairs as **"Waveshare Clicker"**. Tap right = next slide, tap left = previous slide. |
| **Clock** | Analog, digital, or split-flap flip clock — tap (or BOOT) to cycle views. The flip clock clacks on every minute change. |
| **Settings** | Wi-Fi search and connect, with an on-screen keyboard for passwords. The network is saved on the device. |

**Navigation everywhere:** tap a menu item to open it. The menu shows 4
large buttons at a time — **swipe up/down** (or press BOOT) to scroll the
list. **Long-press (hold ~1 s) anywhere returns to the menu.** The side
BOOT button advances / toggles within the current mode.

**Compile-verified 2026-10-25** via PlatformIO (esp32-s3-devkitc-1, Arduino
framework, GFX + SensorLib + core BLE HID + core I2S): build **SUCCESS**,
RAM 21.6%, flash 45.4% for the no-photo build (includes Settings Wi-Fi,
all three clock views, the flip clock's clack sound, scrollable menu,
and sleep). Note: the
sketch deliberately uses no sketch-defined types in function signatures —
Arduino IDE auto-generates prototypes before those types are defined, so
a custom type in a signature fails there even though plain C++ accepts it. The clicker uses the ESP32 core's
built-in BLE HID — no third-party BLE library needed (the popular T-vK
ESP32 BLE Keyboard library does not compile against board package 3.x).
Adding photos grows flash by ~435 KB each.

## What's in this folder

| File | What it is |
|---|---|
| `booth-display-175c.ino` | The app. Open this folder in Arduino IDE. |
| `pin_config.h` | Waveshare's official pin map for the 1.75C (display, touch, audio). |
| `qrcodegen.c` / `qrcodegen.h` | QR generator (Nayuki, MIT) — QR codes are drawn on-device, no images needed. |
| `es8311.c` / `es8311.h` / `es8311_reg.h` | ES8311 speaker-codec driver, from Waveshare's own 1.75C example — drives the flip clock's clack sound. No library install needed; the files travel with the sketch. |
| `convert_photos.py` | Turns your photos into `booth_photos.h` for the sketch. |

## Configuration — fill these in before you flash

Near the top of the sketch:

```cpp
const char *LINKEDIN_URL = "https://www.linkedin.com/in/billgrant";
const char *VCARD_TEXT =
  "MECARD:N:Grant,Bill;"
  "TEL:+1-917-202-9936;"       // <-- your phone
  "EMAIL:bigrant@cisco.com;"    // <-- your email
  "URL:https://www.linkedin.com/in/billgrant;;";
const char *MEETING_URL = "https://calendly.com/your-link";
```

- **LinkedIn URL:** your full profile URL.
- **vCard:** phones scan this straight into their contacts — no app, no
  typing. Keep the MECARD format (semicolons between fields, double
  semicolon at the end). If you send me the details, I'll bake them in.
- **Meeting URL:** your Calendly / Cal.com / HubSpot booking link.
- **Badge text:** `BADGE_TEXT` is already set to "Got evals?".

## Set up Arduino IDE

1. Install Arduino IDE, then add ESP32 board support:
   **File → Preferences → Additional Boards Manager URLs**, add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`,
   then **Tools → Board → Boards Manager** → install **esp32**.
2. Install libraries (**Sketch → Include Library → Manage Libraries**):
   - **GFX Library for Arduino** (moononournation)
   - **SensorLib** (lewis he)
   - No extra library for clicker mode: it uses the ESP32 board package's
     built-in BLE HID support, so you need board package **3.x or newer**
     (the popular "ESP32 BLE Keyboard" library does not compile against
     it — its API predates the core's BLE changes).
3. Open this folder's `.ino` file.
4. **Tools** settings:
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enable**
   - Flash Size: **32MB** (this board has 32MB flash)
   - PSRAM: **OPI PSRAM**
   - Partition Scheme: default is fine without photos; with several photos
     pick a large-APP scheme, e.g. "32MB (16MB APP/15MB FATFS)"
5. Plug the board in over USB-C. If the port doesn't appear, hold **BOOT**,
   tap **PWR**, release BOOT (download mode).
6. Upload. The menu appears.

## Adding your photos

```bash
pip install Pillow
python3 convert_photos.py photo1.jpg photo2.jpg photo3.jpg
```

This writes `booth_photos.h` next to the sketch. Re-upload and your photos
become the flipbook pages (full-screen, in the order you listed them).
The **first photo also becomes the badge-mode picture**. To go back to the
text-only version, delete `booth_photos.h` and re-upload.

Tips:
- The screen is round, so corners get clipped — keep faces and subjects
  near the centre of each photo. The converter centre-crops to a square.
- Each photo costs ~435 KB of flash; a handful is fine on 32 MB.

## Using it at the booth

- **Menu:** tap an item. **Long-press anywhere:** back to menu.
- **Flipbook:** swipe left/right, or tap right 2/3 for next / left 1/3 for
  back. Auto-advances every 10 s (`AUTO_ADVANCE_MS`, 0 = off).
- **QR Codes:** swipe or tap to cycle LinkedIn → contact card → booking.
  The label at the top tells you (and them) which one is showing.
- **Badge:** tap to switch animation ↔ photo. Wear it on a lanyard while
  walking the floor; the animation is the eye-catcher.
- **Clock:** tap or press BOOT to cycle analog → digital → flip. The
  analog face has a bezel, minute track, hour numerals, and thick
  hour/minute hands with tails; the digital view is big HH:MM with
  AM/PM and date; the **flip clock** shows split-flap cards that
  animate each minute and play a mechanical "clack" through the speaker
  (synthesized in the sketch — a noise click plus a resonant thump, no
  audio files needed). Tap the **top-right corner** in flip view to
  mute/unmute; the label there shows the current state. None of the
  clocks show seconds, so the analog and digital faces only redraw when
  the minute changes — no flicker.
  Time source: if Wi-Fi is connected (see Settings below), entering Clock
  mode syncs once via NTP. Otherwise the clock runs from the sketch's
  compile time, so flash soon after compiling and it will be right for
  the day. Timezone is US Eastern (`CLOCK_TZ` at the top of the sketch —
  change it if you travel).
- **Settings → Wi-Fi:** the settings screen shows the connection status
  (connected network + IP, saved network, or not connected). Tap
  **Wi-Fi networks** to scan — networks are listed strongest first, with
  signal bars and a padlock for secured ones. Tap one: open networks
  connect immediately; secured ones open an on-screen keyboard (letters,
  Shift for capitals, `#+=" for symbols, Space, backspace, Done, Cancel).
  On success the network is saved on the device, so it survives reboots
  and the clock's NTP sync uses it automatically. **Forget saved Wi-Fi**
  clears it. BOOT goes back to the settings menu from any Wi-Fi screen.
  (You can still hardcode `WIFI_SSID` / `WIFI_PASS` at the top of the
  sketch instead — that takes priority if filled in.)
- **Settings → Sleep device:** asks for confirmation, then puts the
  board into deep sleep — screen dark, everything off. Only the side
  **BOOT button** wakes it (it restarts to the menu). Your saved Wi-Fi
  survives the sleep; the clock re-syncs via NTP next time you open it
  if a network is saved, otherwise it restarts from compile time.
- **Clicker:** open Clicker mode, then on your laptop pair Bluetooth →
  "Waveshare Clicker". Tap the right half for next slide, left half for
  previous. Long-press on the device returns to the menu (and stops
  advertising). Arrow keys advance almost every slide app (PowerPoint,
  Keynote, Google Slides).
- Keep it on USB power at the booth for all-day use — the screen runs at
  full brightness, which is what you want under show lighting but will
  drain the internal battery faster than usual.
- If touch ever misbehaves, the BOOT button still advances within the
  current mode, and a long-press is only needed to change modes.

## How it works (briefly)

Display init and touch init follow Waveshare's own 1.75C Arduino examples
exactly: CO5300 display over QSPI (CS 12, SCK 38, data 4–7, reset GPIO1)
and CST9217 touch over I2C (SDA 15, SCL 14, INT 11, reset GPIO2). QR codes
are rendered module-by-module from their strings — no Wi-Fi, no account,
and no photo of a QR code needed. Clicker mode uses the ESP32-S3's
Bluetooth HID keyboard support, so no dongle or app is needed on the laptop.
