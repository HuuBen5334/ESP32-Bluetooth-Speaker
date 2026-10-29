#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include <TFT_eSPI.h>
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);   // full-screen off-screen buffer

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);
#define BUTTON_1 35
#define BUTTON_2 0

//============================== UI State ===================================
String icon    = "No device";
String song    = "";
String artist  = "";
bool   isPlaying   = false;
bool   isConnected = false;
volatile bool displayDirty = true;
//===========================================================================

// Colors (RGB565)
#define COL_BG      0x0861   // near-black blue
#define COL_PANEL   0x18E3   // dark grey panel
#define COL_TITLE   TFT_WHITE
#define COL_ARTIST  0xA534   // soft grey
#define COL_ACCENT  TFT_ORANGE
#define COL_OK      0x2E8B   // green
#define COL_BAD     0xC945   // red

volatile bool button1Pressed = false;
volatile bool button2Pressed = false;
unsigned long lastDebounceTime1 = 0;
unsigned long lastDebounceTime2 = 0;
const unsigned long debounceDelay = 200;

void IRAM_ATTR isrButton1() { button1Pressed = true; }
void IRAM_ATTR isrButton2() { button2Pressed = true; }

//========================= Bluetooth callbacks =============================
void audio_state_changed(esp_a2d_audio_state_t state, void *ptr) {
    isPlaying = (state == ESP_A2D_AUDIO_STATE_STARTED);
    displayDirty = true;
}

void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
    isConnected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
    icon = isConnected ? "Connected" : "No device";
    if (!isConnected) { song = ""; artist = ""; isPlaying = false; }
    displayDirty = true;
}

void metadata_callback(uint8_t id, const uint8_t *text) {
    Serial.printf("ID: %d -> %s\n", id, (const char*)text);
    if (id == ESP_AVRC_MD_ATTR_TITLE)  { song   = (const char*)text; displayDirty = true; }
    if (id == ESP_AVRC_MD_ATTR_ARTIST) { artist = (const char*)text; displayDirty = true; }
}

//========================= Drawing helpers =================================
// Truncate a string to fit maxWidth pixels in the given font, adding "..."
String fitText(String s, uint8_t font, int maxWidth) {
    if (spr.textWidth(s, font) <= maxWidth) return s;
    while (s.length() > 0 && spr.textWidth(s + "...", font) > maxWidth)
        s.remove(s.length() - 1);
    return s + "...";
}

// Generated "album art": gradient + vinyl disc, colors hashed from track info
void drawCoverArt(int x, int y, int size) {
    // FNV-1a hash of title+artist -> stable per-track colors
    uint32_t h = 2166136261u;
    String key = song + artist;
    for (unsigned int i = 0; i < key.length(); i++) { h ^= (uint8_t)key[i]; h *= 16777619u; }
    if (key.length() == 0) h = 0x334455;  // idle color

    uint8_t r1 = 40 + (h & 0x7F),  g1 = 40 + ((h >> 7) & 0x7F),  b1 = 40 + ((h >> 14) & 0x7F);
    uint8_t r2 = r1 / 3, g2 = g1 / 3, b2 = b1 / 3;

    // Vertical gradient
    for (int i = 0; i < size; i++) {
        uint8_t r = r1 + (int)(r2 - r1) * i / size;
        uint8_t g = g1 + (int)(g2 - g1) * i / size;
        uint8_t b = b1 + (int)(b2 - b1) * i / size;
        spr.drawFastHLine(x, y + i, size, spr.color565(r, g, b));
    }

    // Vinyl disc
    int cx = x + size / 2, cy = y + size / 2;
    spr.fillCircle(cx, cy, size / 2 - 8, TFT_BLACK);
    spr.drawCircle(cx, cy, size / 2 - 12, 0x39E7);
    spr.drawCircle(cx, cy, size / 2 - 18, 0x39E7);
    spr.fillCircle(cx, cy, 9, spr.color565(r1, g1, b1));
    spr.fillCircle(cx, cy, 2, TFT_BLACK);

    spr.drawRect(x, y, size, size, COL_PANEL);  // frame
}

void drawPlayPause(int x, int y) {
    if (isPlaying) {   // pause: two bars
        spr.fillRoundRect(x,     y, 5, 16, 1, COL_ACCENT);
        spr.fillRoundRect(x + 9, y, 5, 16, 1, COL_ACCENT);
    } else {           // play: triangle
        spr.fillTriangle(x, y, x, y + 16, x + 14, y + 8, COL_ACCENT);
    }
}

void draw() {
    spr.fillSprite(COL_BG);

    // --- Header bar ---
    spr.fillRect(0, 0, 240, 18, COL_PANEL);
    spr.setTextColor(COL_ARTIST, COL_PANEL);
    spr.drawString("MyMusic", 6, 3, 2);
    spr.fillCircle(228, 9, 4, isConnected ? COL_OK : COL_BAD);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(icon, 220, 3, 2);
    spr.setTextDatum(TL_DATUM);

    // --- Album art (left) ---
    drawCoverArt(8, 26, 100);

    // --- Track info (right) ---
    int tx = 118, tw = 240 - tx - 6;
    spr.setTextColor(COL_TITLE, COL_BG);
    spr.drawString(song.length()   ? fitText(song, 2, tw)   : "-", tx, 34, 2);
    spr.setTextColor(COL_ARTIST, COL_BG);
    spr.drawString(artist.length() ? fitText(artist, 2, tw) : "-", tx, 56, 2);

    // --- Play state ---
    drawPlayPause(tx, 96);
    spr.setTextColor(COL_ACCENT, COL_BG);
    spr.drawString(isPlaying ? "Playing" : "Paused", tx + 24, 96, 2);

    spr.pushSprite(0, 0);
}

//================================ Setup ====================================
void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    spr.createSprite(240, 135);
    if (spr.getPointer() == nullptr) {
        Serial.println("Sprite alloc failed!");  // out of heap
    }

    pinMode(BUTTON_1, INPUT);          // external pull-up on T-Display
    pinMode(BUTTON_2, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_1), isrButton1, FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_2), isrButton2, FALLING);

    auto cfg = i2s.defaultConfig();
    cfg.pin_bck = 25;
    cfg.pin_ws = 27;
    cfg.pin_data = 26;
    i2s.begin(cfg);

    a2dp_sink.set_avrc_metadata_attribute_mask(
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
    a2dp_sink.set_avrc_metadata_callback(metadata_callback);
    a2dp_sink.set_on_connection_state_changed(connection_state_changed);
    a2dp_sink.set_on_audio_state_changed(audio_state_changed);
    a2dp_sink.start("MyMusic");
}

//================================ Loop =====================================
void loop() {
    unsigned long now = millis();

    if (button1Pressed) {
        button1Pressed = false;
        if (now - lastDebounceTime1 > debounceDelay) {
            lastDebounceTime1 = now;
            if (isPlaying) {
                a2dp_sink.pause();
                isPlaying = false;
            } else {
                a2dp_sink.play();
                isPlaying = true;
            }
            displayDirty = true;
        }
    }

    if (button2Pressed) {
        button2Pressed = false;
        if (now - lastDebounceTime2 > debounceDelay) {
            lastDebounceTime2 = now;
            a2dp_sink.next();   // second button = skip track
        }
    }

    if (displayDirty) {
        displayDirty = false;
        draw();
    }

    delay(10);
}