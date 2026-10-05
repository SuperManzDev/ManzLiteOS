// ============================================================
//  ManzLiteOS v7.0  —  by Manz
//  ESP32 + 16x2 I2C LCD Embedded OS
// ============================================================
//
//  CHANGES IN v7.0:
//   - ANCS CLIENT: Phone Notify now uses native iOS ANCS.
//     Pair ESP32 in iPhone BT Settings to get notifications.
//   - EMOJI CRASH FIX: Sanitizer strips emojis/control chars
//     before writing to the 16x2 LCD (was crashing before).
//   - BLE SCANNER: Rewrote using standard ESP32 BLEDevice API.
//     Shows device name instead of raw MAC sub-string.
//   - ANCS resets cleanly after BLE Scanner so Phone Notify
//     can re-pair on next launch.
//
//  PHONE NOTIFICATION SETUP:
//  ─────────────────────────────────────────────────────────
//  The ESP32 hosts a BLE GATT server. Your phone connects and
//  writes notification data to these characteristics:
//
//   Service UUID:    4fafc201-1fb5-459e-8fcc-c5c9c331914b
//   Notif Char UUID: beb5483e-36e1-4688-b7f5-ea07361b26a8
//   Music Char UUID: beb5483e-36e1-4688-b7f5-ea07361b26a9
//
//  Message format (pipe-separated, max 60 chars):
//   Notifications:  TYPE|SENDER|MESSAGE
//                   e.g. "SMS|Mom|Hey are you home?"
//                        "IG|john_doe|liked your photo"
//                        "CALL|Dad|Incoming call"
//   Music:          MUSIC|ARTIST|SONG TITLE
//                   e.g. "MUSIC|Drake|Gods Plan"
//
//  ANDROID SETUP (Tasker + AutoNotification):
//   1. Install Tasker + AutoNotification plugin
//   2. Create profile: Event > Plugin > AutoNotification Intercept
//   3. Action: BLE GATT Write to "ManzOS"
//      Service: 4fafc201-1fb5-459e-8fcc-c5c9c331914b
//      Char:    beb5483e-36e1-4688-b7f5-ea07361b26a8
//      Value:   %an_app|%an_title|%an_text
//
//  ANDROID SETUP (MacroDroid - simpler):
//   1. Trigger: Notification Received
//   2. Action: BLE Peripheral Write (search MacroDroid BLE)
//      Device: ManzOS
//      Value: [App]|[Title]|[Text]
//
//  iOS SETUP (Shortcuts app):
//   Use "Send BLE Message" shortcut action (requires BLE shortcut)
//   Or use "nRF Connect" app to manually test writes
//
//  QUICK TEST (any platform):
//   Use "nRF Connect" or "LightBlue" app:
//   1. Scan for "ManzOS"
//   2. Connect
//   3. Find the service UUID above
//   4. Write to notification char: "SMS|Test|Hello from phone!"
//   5. Watch the LCD scroll it!
//  ─────────────────────────────────────────────────────────
//
//  PIN ASSIGNMENTS
//   BTN_UP    = 13   BTN_DOWN  = 14
//   BTN_LEFT  = 25   BTN_RIGHT = 26
//   BTN_OK    = 27
//   BUZZ1     = 33   (ACTIVE buzzer — digitalWrite HIGH/LOW)
//   BUZZ2     = 23   (PASSIVE buzzer — ledcWriteTone)
//   BATT_PIN  = 34   (ADC battery voltage divider)
//   LCD SDA   = 21   LCD SCL   = 22
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "esp32notifications.h"
#include <time.h>

// ── Hardware ─────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
AsyncWebServer webServer(80);

#define BTN_UP    13
#define BTN_DOWN  14
#define BTN_LEFT  25
#define BTN_RIGHT 26
#define BTN_OK    27
#define BUZZ1     33
#define BUZZ2     23
#define BATT_PIN  34

// ── EEPROM Layout ────────────────────────────────────────────
#define EEPROM_SIZE       80
#define ADDR_PIN_SET       0
#define ADDR_PIN_DIGITS    1
#define ADDR_AUDIO_MUTE    5
#define ADDR_SLEEP_TIMEOUT 6
#define ADDR_POMO_COUNT   60

// ── WiFi HARDCODED FOR TESTING ────────────────────────────────
#define WIFI_SSID  "ATTRKbdFEa ext 2.4"
#define WIFI_PASS  "iem%qmiz#y9v"
#define OWM_API_KEY  "2cb9dca33881e5504f4cb976cff7f793"
#define OWM_CITY     "Detroit"
#define OWM_COUNTRY  "US"
#define NTP_SERVER   "pool.ntp.org"
#define TZ_OFFSET_SEC -18000

// (ANCS client — no GATT UUIDs needed)

// ── OS State ─────────────────────────────────────────────────
bool  audioMuted    = false;
bool  backlightOn   = true;
bool  wifiConnected = false;
bool  ntpSynced     = false;
unsigned long lastActivity = 0;
unsigned long systemBoot   = 0;
int sleepOptions[] = {5,10,15,30,60,0};
int sleepOptionIdx = 3;

// ── ANCS State ───────────────────────────────────────────────
// (globals defined below forward declarations)

// ── Button State (FIXED DEBOUNCE) ────────────────────────────
unsigned long btnLastPress[5]   = {0,0,0,0,0};
bool          btnWasReleased[5] = {true,true,true,true,true};
#define BTN_DEBOUNCE_MS 80

// ── Custom LCD Chars ─────────────────────────────────────────
byte arrowUp[8]    = {0b00100,0b01110,0b11111,0b00100,0b00100,0b00100,0b00100,0b00000};
byte arrowDown[8]  = {0b00100,0b00100,0b00100,0b00100,0b11111,0b01110,0b00100,0b00000};
byte blockFull[8]  = {0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111};
byte dinoBody[8]   = {0b00110,0b00111,0b11110,0b11111,0b01110,0b01110,0b01010,0b01010};
byte cactus[8]     = {0b00100,0b10101,0b11111,0b00100,0b00100,0b00100,0b00100,0b01110};
byte paddleChar[8] = {0b00000,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b00000};
byte ballChar[8]   = {0b00000,0b01110,0b11111,0b11111,0b11111,0b01110,0b00000,0b00000};
byte battChar[8]   = {0b01110,0b11111,0b10001,0b10001,0b10001,0b11111,0b11111,0b11111};

// ── Pixel Draw Canvas ─────────────────────────────────────────
uint8_t drawCanvas[2][8][8];
bool    drawDirty = false;

// ── Forward Declarations ──────────────────────────────────────
void lcdPrint(int row, const char* left, const char* right = "");
void lcdPrintCenter(int row, const char* text);
void lcdClear();
void activeBuzz(int dur);
void beep(int freq, int dur);
void chord(int f1, int f2, int dur);
void playRingtone();
void activity();
void checkSleep();
bool btnPressed(int pin);
void connectWiFi();
int  readBatteryPct();
void restoreCustomChars();
void bootSequence();
void lockScreen();
void mainMenu();
void launchApp(int idx);
void appPhoneNotify();
void appSystemInfo();
void appStopwatch();
void appFocusTimer();
void appTallyCounter();
void appDualSynth();
void appClock();
void appWeather();
void appWiFiScanner();
void appBLEScanner();
void appI2CScanner();
void appPasswordGen();
void appReactionTest();
void appSimonSays();
void appDinoRun();
void appPong();
void appGameOfLife();
void appMatrixRain();
void appLoadingSnake();
void appWebDashboard();
void appPixelDraw();
void appTOTP();
void appPomodoroStats();
void appSettings();
void settingSetPIN();
void appDemoMode();
// ─────────────────────────────────────────────────────────────
//  ANCS CLIENT STATE  (BLENotifications — Smartphone-Companions)
// ─────────────────────────────────────────────────────────────
BLENotifications notifications;
static bool ancsInitialized  = false;
static bool ancsConnected    = false;
volatile bool ancsNotifReady = false;
static bool lastWasMusic     = false;  // true when last notif was entertainment
static int  ancsViewMode     = 0;      // 0=notifications  1=Now Playing

// Notification display buffers (written from callback, read from main loop)
char ancsApp[32]    = "No notifs yet";
char ancsRaw[200]   = {0};   // sanitized title + message for row1 scroll
char musicRaw[200]  = {0};   // sanitized artist+track for music view
bool hasMusicInfo   = false;

// ── Helper: sanitize a string in-place (strip emojis / ctrl chars) ──
static void sanitize(char* buf) {
  for (int i = 0; buf[i] != '\0'; i++) {
    if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126) buf[i] = ' ';
  }
}

// ── BLE state callback ───────────────────────────────────────
void onBLEStateChanged(BLENotifications::State state, const void* userData) {
  if (state == BLENotifications::StateConnected) {
    ancsConnected = true;
  } else if (state == BLENotifications::StateDisconnected) {
    ancsConnected = false;
    notifications.startAdvertising();
  }
}

// ── Category → friendly app label ───────────────────────────
static const char* categoryLabel(NotificationCategory cat) {
  switch (cat) {
    case CategoryIDIncomingCall:       return "[CALL]";
    case CategoryIDMissedCall:         return "[MISS]";
    case CategoryIDSocial:             return "[Social]";
    case CategoryIDEmail:              return "[Email]";
    case CategoryIDNews:               return "[News]";
    case CategoryIDEntertainment:      return "[Music]";
    case CategoryIDHealthAndFitness:   return "[Health]"; 
    case CategoryIDLocation:           return "[Maps]";
    default:                           return "[iOS]";
  }
}

// ── Notification arrived callback ───────────────────────────
void onNotificationArrived(const Notification* notification, const void* userData) {
  if (!notification) return;

  // App label from category
  strncpy(ancsApp, categoryLabel(notification->category), 31);
  ancsApp[31] = 0;

  // Build sanitized title - message string
  char tmp[200] = {0};
  snprintf(tmp, sizeof(tmp), "%s - %s",
           notification->title.c_str(),
           notification->message.c_str());
  sanitize(tmp);
  strncpy(ancsRaw, tmp, 199); ancsRaw[199] = 0;

  // If entertainment (music), also save to music view
  lastWasMusic = (notification->category == CategoryIDEntertainment);
  if (lastWasMusic) {
    strncpy(musicRaw, tmp, 199); musicRaw[199] = 0;
    hasMusicInfo = true;
  }

  ancsNotifReady = true;
}

// ─────────────────────────────────────────────────────────────
//  AMS STREAMING STATE  (Apple Media Service — true Now Playing)
// ─────────────────────────────────────────────────────────────
char amsArtist[64]    = {0};
char amsTitle[64]     = {0};
char amsPlayState[2]  = "?";  // ">" playing  "|" paused  "?" unknown
volatile bool amsUpdated = false;

// Helper: rebuild musicRaw from latest AMS artist + title (sanitized)
static void rebuildMusicRaw() {
  char tmp[200] = {0};
  if (amsArtist[0] && amsTitle[0]) {
    snprintf(tmp, sizeof(tmp), "%s  -  %s", amsArtist, amsTitle);
  } else if (amsTitle[0]) {
    snprintf(tmp, sizeof(tmp), "%s", amsTitle);
  } else if (amsArtist[0]) {
    snprintf(tmp, sizeof(tmp), "%s", amsArtist);
  } else {
    snprintf(tmp, sizeof(tmp), "No track info");
  }
  sanitize(tmp);
  strncpy(musicRaw, tmp, 199); musicRaw[199] = 0;
  hasMusicInfo = true;
  amsUpdated = true;
}

// ── AMS Track callback (fires on every artist/title/album change) ──
void onAMSTrackUpdate(const AMSTrackAttributeID_t attr, const std::string& value, const void*) {
  char sanitized[64] = {0};
  strncpy(sanitized, value.c_str(), 63);
  sanitize(sanitized);
  switch (attr) {
    case AMSTrackAttributeIDArtist: strncpy(amsArtist, sanitized, 63); break;
    case AMSTrackAttributeIDTitle:  strncpy(amsTitle,  sanitized, 63); break;
    default: return;  // ignore album / duration for now
  }
  rebuildMusicRaw();
}

// ── AMS Player callback (fires on play/pause state changes) ──
void onAMSPlayerUpdate(const AMSPlayerAttributeID_t attr, const std::string& value, const void*) {
  if (attr == AMSPlayerAttributeIDPlaybackInfo && value.length() > 0) {
    // PlaybackInfo format: "PlaybackState,Rate,ElapsedTime"
    // State: 0=paused  1=playing  2=rewinding  3=fast-fwd
    switch (value[0]) {
      case '0': amsPlayState[0] = '|'; break;  // paused
      case '1': amsPlayState[0] = '>'; break;  // playing
      case '2': amsPlayState[0] = '<'; break;  // rewinding
      case '3': amsPlayState[0] = '>'; break;  // fast-fwd (show as play)
      default:  amsPlayState[0] = '?'; break;
    }
    amsUpdated = true;
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(400);
  Serial.println("\n=== MANZLITE OS v7.0 ===");
  EEPROM.begin(EEPROM_SIZE);
  audioMuted     = EEPROM.read(ADDR_AUDIO_MUTE);
  sleepOptionIdx = EEPROM.read(ADDR_SLEEP_TIMEOUT);
  if (sleepOptionIdx>5) sleepOptionIdx=3;
  pinMode(BTN_UP,INPUT_PULLUP); pinMode(BTN_DOWN,INPUT_PULLUP);
  pinMode(BTN_LEFT,INPUT_PULLUP); pinMode(BTN_RIGHT,INPUT_PULLUP);
  pinMode(BTN_OK,INPUT_PULLUP);
  pinMode(BUZZ1,OUTPUT); digitalWrite(BUZZ1,LOW);
  ledcSetup(0,2000,8); ledcAttachPin(BUZZ2,0);
  pinMode(BATT_PIN,INPUT); analogReadResolution(12);
  lcd.init(); lcd.backlight();
  restoreCustomChars();
  memset(drawCanvas,0,sizeof(drawCanvas));
  systemBoot=lastActivity=millis();
  bootSequence();
  if (EEPROM.read(ADDR_PIN_SET)==0x42) lockScreen();
}

void loop() { checkSleep(); mainMenu(); }

// ─────────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────────
void restoreCustomChars() {
  lcd.createChar(0,arrowUp); lcd.createChar(1,arrowDown);
  lcd.createChar(2,blockFull); lcd.createChar(3,paddleChar);
  lcd.createChar(4,ballChar); lcd.createChar(5,dinoBody);
  lcd.createChar(6,cactus); lcd.createChar(7,battChar);
}

int readBatteryPct() {
  long s=0; for(int i=0;i<8;i++){s+=analogRead(BATT_PIN);delay(2);}
  float v=(s/8.0f/4095.0f)*3.3f*2.0f;
  int p=(int)((v-3.2f)*100.0f); if(p>100)p=100; if(p<0)p=0; return p;
}

void bootSequence() {
  lcdClear(); lcd.setCursor(2,0); lcd.print("ManzLiteOS");
  lcd.setCursor(5,1); lcd.print("v7.0");
  int j[]={880,1109,1318,1760}; int jl[]={70,70,70,120};
  for(int i=0;i<4;i++){beep(j[i],jl[i]);delay(30);} delay(300);
  lcdClear(); lcd.setCursor(0,0); lcd.print(" Initializing...");
  for(int i=0;i<16;i++){lcd.setCursor(i,1);lcd.write(byte(2));delay(55);}
  delay(250);
}

void lockScreen() {
  int sp[4]; for(int i=0;i<4;i++)sp[i]=EEPROM.read(ADDR_PIN_DIGITS+i);
  int att=0;
  while(true){
    int en[4]={0,0,0,0};
    lcdClear(); lcd.setCursor(0,0); lcd.print("PIN: _ _ _ _    "); lcd.setCursor(0,1); lcd.print("UP/DN:val OK:ok ");
    for(int pos=0;pos<4;){
      activity();
      if(btnPressed(BTN_UP))  {en[pos]=(en[pos]+1)%10;lcd.setCursor(5+pos*2,0);lcd.print(en[pos]);activeBuzz(25);}
      if(btnPressed(BTN_DOWN)){en[pos]=(en[pos]+9)%10;lcd.setCursor(5+pos*2,0);lcd.print(en[pos]);activeBuzz(25);}
      if(btnPressed(BTN_OK)) {lcd.setCursor(5+pos*2,0);lcd.print(en[pos]);pos++;if(pos<4){lcd.setCursor(5+pos*2,0);lcd.print("_");}beep(1400,35);}
      delay(18);
    }
    bool ok=true; for(int i=0;i<4;i++)if(en[i]!=sp[i])ok=false;
    if(ok){lcdClear();lcdPrintCenter(0,"** UNLOCKED **");lcdPrintCenter(1,"Welcome back!");chord(1318,1760,100);delay(600);return;}
    else{
      att++;
      if(att>=3){lcdClear();lcd.setCursor(0,0);lcd.print("!! LOCKED OUT !!");lcd.setCursor(0,1);lcd.print("Wait 60 seconds ");for(int i=0;i<8;i++){activeBuzz(80);beep(300,80);delay(100);}delay(60000);att=0;}
      else{lcdClear();lcd.setCursor(0,0);lcd.print(" Wrong PIN!     ");char b[17];snprintf(b,17," %d attempt(s)   ",att);lcd.setCursor(0,1);lcd.print(b);activeBuzz(200);delay(1200);}
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  MAIN MENU
// ─────────────────────────────────────────────────────────────
const char* menuItems[] = {
  "Phone Notify", "System Info",  "Stopwatch",
  "Focus Timer",  "Tally Counter","Dual Synth",
  "Clock",        "Weather",      "Wi-Fi Scanner",
  "BLE Scanner",  "I2C Scanner",  "Password Gen",
  "Reaction Test","Simon Says",   "Dino Run",
  "Pong",         "Game of Life", "Matrix Rain",
  "Load Snake",   "Web Dashboard","Pixel Draw",
  "TOTP 2FA",     "Pomo Stats",   "Settings",
  "Demo Mode",    "Lock Screen"
};
const int MENU_COUNT=26;

void mainMenu() {
  static int mi=0;
  while(true){
    activity(); lcdClear();
    char top[15],bot[15];
    strncpy(top,menuItems[mi],14);top[14]=0;
    strncpy(bot,menuItems[(mi+1)%MENU_COUNT],14);bot[14]=0;
    lcd.setCursor(0,0);lcd.print("> ");lcd.print(top);
    for(int i=strlen(top)+2;i<15;i++)lcd.print(' ');lcd.write(byte(0));
    lcd.setCursor(0,1);lcd.print("  ");lcd.print(bot);
    for(int i=strlen(bot)+2;i<16;i++)lcd.print(' ');
    delay(90);
    while(true){
      activity();
      if(btnPressed(BTN_UP))  {mi=(mi-1+MENU_COUNT)%MENU_COUNT;beep(1200,16);break;}
      if(btnPressed(BTN_DOWN)){mi=(mi+1)%MENU_COUNT;            beep(1200,16);break;}
      if(btnPressed(BTN_OK)||btnPressed(BTN_RIGHT)){beep(1600,26);launchApp(mi);break;}
      delay(14);
    }
  }
}

void launchApp(int idx){
  switch(idx){
    case 0:  appPhoneNotify();   break;
    case 1:  appSystemInfo();    break;
    case 2:  appStopwatch();     break;
    case 3:  appFocusTimer();    break;
    case 4:  appTallyCounter();  break;
    case 5:  appDualSynth();     break;
    case 6:  appClock();         break;
    case 7:  appWeather();       break;
    case 8:  appWiFiScanner();   break;
    case 9:  appBLEScanner();    break;
    case 10: appI2CScanner();    break;
    case 11: appPasswordGen();   break;
    case 12: appReactionTest();  break;
    case 13: appSimonSays();     break;
    case 14: appDinoRun();       break;
    case 15: appPong();          break;
    case 16: appGameOfLife();    break;
    case 17: appMatrixRain();    break;
    case 18: appLoadingSnake();  break;
    case 19: appWebDashboard();  break;
    case 20: appPixelDraw();     break;
    case 21: appTOTP();          break;
    case 22: appPomodoroStats(); break;
    case 23: appSettings();      break;
    case 24: appDemoMode();      break;
    case 25: lockScreen();       break;
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: PHONE + MUSIC  (ANCS — BLENotifications library)
// ─────────────────────────────────────────────────────────────
void appPhoneNotify() {
  // Init BLENotifications + AMS once per session
  if (!ancsInitialized) {
    notifications.begin("ManzOS");
    notifications.setConnectionStateChangedCallback(onBLEStateChanged);
    notifications.setNotificationCallback(onNotificationArrived);
    // ── Register AMS callbacks (ddlsmurf fork) ──────────────────
    notifications.setOnAMSTrackUpdateCB(onAMSTrackUpdate);
    notifications.setOnAMSPlayerUpdateCB(onAMSPlayerUpdate);
    ancsInitialized = true;
  }

  // Splash
  lcdClear();
  lcd.setCursor(0,0); lcd.print("Phone + Music   ");
  lcd.setCursor(0,1); lcd.print(ancsConnected ? "ANCS Connected  " : "Pair in Settings");
  delay(1200);

  // ── Scroll state ──────────────────────────────────────────────
  char scrollBuf[220]      = {0};
  int  scrollOff           = 0;
  unsigned long lastScroll = 0;
  bool needRebuild         = true;

  auto buildScroll = [&]() {
    const char* src = (ancsViewMode == 1 && hasMusicInfo) ? musicRaw : ancsRaw;
    if (!src || src[0] == '\0') {
      snprintf(scrollBuf, sizeof(scrollBuf), "%s",
               (ancsViewMode == 1) ? "No music yet...  " : "No notifs yet... ");
    } else {
      snprintf(scrollBuf, sizeof(scrollBuf), "%-s        ", src);
    }
    scrollOff   = 0;
    needRebuild = false;
    amsUpdated  = false;
  };
  buildScroll();

  while (true) {
    activity();

    // ── AMS streaming update (track/artist changed) ───────────────
    if (amsUpdated) {
      amsUpdated  = false;
      needRebuild = true;
      // Auto-switch to music view on fresh AMS data
      if (ancsViewMode != 1) { ancsViewMode = 1; beep(1047,35); delay(18); beep(1319,55); }
    }

    // ── ANCS notification arrived ─────────────────────────────────
    if (ancsNotifReady) {
      ancsNotifReady = false;
      if (!backlightOn) { lcd.backlight(); backlightOn = true; }
      if (lastWasMusic) {
        ancsViewMode = 1;
        beep(1047,35); delay(18); beep(1319,55);
      } else {
        ancsViewMode = 0;
        playRingtone();
      }
      needRebuild = true;
    }

    if (needRebuild) buildScroll();

    // ── Row 0: status bar ─────────────────────────────────────────
    char row0[17] = {0};
    if (!ancsConnected) {
      snprintf(row0, 17, "ANCS: Pairing..");
    } else if (ancsViewMode == 0) {
      snprintf(row0, 17, "%-16s", ancsApp);
    } else {
      // Music view: show playback state symbol + label
      // e.g.  "> Now Playing:  "  or  "| Now Playing:  "
      snprintf(row0, 17, "%c Now Playing:  ", amsPlayState[0]);
    }

    // ── Row 1: smooth scroll ──────────────────────────────────────
    int slen = strlen(scrollBuf);
    if (slen < 1) slen = 1;

    if (millis() - lastScroll > 350) {
      lastScroll = millis();
      lcdClear();
      lcd.setCursor(0,0); lcd.print(row0);
      lcd.setCursor(0,1);
      for (int i = 0; i < 16; i++) {
        lcd.print(scrollBuf[(scrollOff + i) % slen]);
      }
      scrollOff = (scrollOff + 1) % slen;
    }

    // ── Buttons ───────────────────────────────────────────────────
    if (btnPressed(BTN_UP) || btnPressed(BTN_DOWN)) {
      ancsViewMode = 1 - ancsViewMode;
      needRebuild  = true;
      beep(1200, 18);
    }
    if (btnPressed(BTN_OK)) {
      // Play/Pause toggle
      if (ancsConnected) {
        notifications.amsCommand(AMSRemoteCommandIDTogglePlayPause);
        beep(1047, 28);
      } else {
        notifications.startAdvertising();
        lcdClear(); lcd.setCursor(0,0); lcd.print("Re-advertising..");
        delay(700);
      }
    }
    if (btnPressed(BTN_RIGHT) && ancsConnected) {
      // Next track
      notifications.amsCommand(AMSRemoteCommandIDNextTrack);
      beep(1319, 28);
    }
    if (btnPressed(BTN_LEFT)) return;

    delay(40);
  }
}

void playRingtone(){
  if(audioMuted)return;
  int n[]={1047,1319,1568,1319,1047}; int d[]={75,75,110,75,110};
  for(int i=0;i<5;i++){
    ledcWriteTone(0,n[i]);
    if(i==0||i==2)digitalWrite(BUZZ1,HIGH);
    delay(d[i]); ledcWriteTone(0,0); digitalWrite(BUZZ1,LOW); delay(28);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: SYSTEM INFO
// ─────────────────────────────────────────────────────────────
void appSystemInfo(){
  int page=0; const int P=4;
  while(true){
    activity(); lcdClear();
    unsigned long u=(millis()-systemBoot)/1000;
    char b0[17],b1[17];
    switch(page){
      case 0: snprintf(b0,17,"Up  %02lu:%02lu:%02lu    ",u/3600,(u%3600)/60,u%60); snprintf(b1,17,"RAM %5luB free  ",ESP.getFreeHeap()); break;
      case 1: snprintf(b0,17,"CPU  240 MHz    "); snprintf(b1,17,"Flash %4dKB    ",ESP.getFlashChipSize()/1024); break;
      case 2:{int bt=readBatteryPct();snprintf(b0,17,"Battery: %3d%%   ",bt);char bar[11]="          ";for(int i=0;i<bt*10/100;i++)bar[i]=(char)255;bar[10]=0;snprintf(b1,17,"[%-10s]    ",bar);break;}
      case 3: snprintf(b0,17,ancsInitialized?"ANCS: Active    ":"ANCS: Inactive  "); snprintf(b1,17,wifiConnected?"WiFi: Connected ":"WiFi: Off       "); break;
    }
    lcd.setCursor(0,0);lcd.print(b0); lcd.setCursor(0,1);lcd.print(b1);
    delay(350);
    if(btnPressed(BTN_LEFT))return;
    if(btnPressed(BTN_UP))  {page=(page-1+P)%P;beep(1200,16);}
    if(btnPressed(BTN_DOWN)){page=(page+1)%P;  beep(1200,16);}
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: STOPWATCH
// ─────────────────────────────────────────────────────────────
void appStopwatch(){
  bool run=false; unsigned long st=0,el=0;
  while(true){
    activity();
    unsigned long now=run?el+(millis()-st):el;
    char buf[17]; snprintf(buf,17,"  %02lu:%02lu.%01lu       ",(now/60000)%60,(now/1000)%60,(now%1000)/100);
    lcdClear(); lcd.setCursor(0,0);lcd.print(buf);
    lcd.setCursor(0,1);lcd.print(run?" OK:stop  <:rst ":" OK:start <:rst ");
    if(btnPressed(BTN_OK)){if(!run){st=millis();run=true;}else{el+=millis()-st;run=false;}activeBuzz(28);}
    if(btnPressed(BTN_LEFT)&&!run)return;
    if(btnPressed(BTN_RIGHT)&&!run){el=0;activeBuzz(55);}
    delay(38);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: FOCUS TIMER
// ─────────────────────────────────────────────────────────────
void appFocusTimer(){
  unsigned long dur=25UL*60*1000,st=0; bool run=false;
  while(true){
    activity();
    unsigned long rem=dur;
    if(run){
      unsigned long el=millis()-st;
      if(el>=dur){
        uint16_t p=((uint16_t)EEPROM.read(ADDR_POMO_COUNT))|((uint16_t)EEPROM.read(ADDR_POMO_COUNT+1)<<8);
        p++; EEPROM.write(ADDR_POMO_COUNT,p&0xFF); EEPROM.write(ADDR_POMO_COUNT+1,(p>>8)&0xFF); EEPROM.commit();
        for(int i=0;i<6;i++){if(!backlightOn){lcd.backlight();backlightOn=true;}chord(880,1109,150);lcdClear();lcd.setCursor(0,0);lcd.print("  TIME IS UP!   ");char sc[17];snprintf(sc,17,"  Session #%-4d ",p);lcd.setCursor(0,1);lcd.print(sc);if(btnPressed(BTN_OK)||btnPressed(BTN_LEFT))break;delay(100);}
        run=false; dur=25UL*60*1000; continue;
      }
      rem=dur-el;
    }
    char buf[17]; snprintf(buf,17,"Focus: %02lu:%02lu    ",rem/60000,(rem/1000)%60);
    lcdClear(); lcd.setCursor(0,0);lcd.print(buf);
    lcd.setCursor(0,1);lcd.print(run?" OK:pause <:exit":" OK:start <:exit");
    if(btnPressed(BTN_OK)){if(!run){st=millis();run=true;beep(1600,55);}else{dur=rem;run=false;beep(800,55);}}
    if(btnPressed(BTN_LEFT))return;
    delay(140);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: TALLY COUNTER
// ─────────────────────────────────────────────────────────────
void appTallyCounter(){
  int c=0;
  while(true){
    activity();lcdClear();
    char buf[17];snprintf(buf,17,"  Count: %-5d  ",c);
    lcd.setCursor(0,0);lcd.print(buf);lcd.setCursor(0,1);lcd.print("UP:+1 DN:-1 >:0 ");
    if(btnPressed(BTN_UP))  {c++;activeBuzz(22);delay(95);}
    if(btnPressed(BTN_DOWN)){c--;activeBuzz(22);delay(95);}
    if(btnPressed(BTN_RIGHT)||btnPressed(BTN_OK)){c=0;activeBuzz(55);}
    if(btnPressed(BTN_LEFT))return;
    delay(22);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: DUAL SYNTH
// ─────────────────────────────────────────────────────────────
void appDualSynth(){
  lcdClear();lcd.setCursor(0,0);lcd.print("  DUAL SYNTH    ");lcd.setCursor(0,1);lcd.print("U/D/R OK:chord  ");delay(700);
  int notes[]={523,659,784,1047,1318};
  static unsigned long lh=0;
  while(true){
    activity();
    if(!audioMuted){
      if     (digitalRead(BTN_UP)==LOW)   {ledcWriteTone(0,notes[0]);}
      else if(digitalRead(BTN_DOWN)==LOW) {ledcWriteTone(0,notes[1]);}
      else if(digitalRead(BTN_RIGHT)==LOW){ledcWriteTone(0,notes[2]);}
      else if(digitalRead(BTN_OK)==LOW)   {ledcWriteTone(0,notes[0]);digitalWrite(BUZZ1,HIGH);}
      else                                {ledcWriteTone(0,0);digitalWrite(BUZZ1,LOW);}
    }
    if(digitalRead(BTN_LEFT)==LOW){if(lh==0)lh=millis();if(millis()-lh>900){ledcWriteTone(0,0);digitalWrite(BUZZ1,LOW);return;}}else lh=0;
    delay(7);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: CLOCK
// ─────────────────────────────────────────────────────────────
void appClock(){
  if(!ntpSynced){
    lcdClear();lcd.setCursor(0,0);lcd.print("Syncing NTP...  ");lcd.setCursor(0,1);lcd.print("Please wait...  ");
    connectWiFi();
    if(wifiConnected){configTime(TZ_OFFSET_SEC,0,NTP_SERVER);delay(2000);ntpSynced=true;}
    else{lcdClear();lcd.setCursor(0,0);lcd.print(" WiFi failed!   ");delay(2000);return;}
  }
  while(true){
    activity(); struct tm ti;
    if(!getLocalTime(&ti)){lcdClear();lcd.setCursor(0,0);lcd.print("  Time Error!   ");delay(1000);}
    else{char t0[17],t1[17];strftime(t0,17,"    %H:%M:%S    ",&ti);strftime(t1,17,"  %a %d %b %y  ",&ti);lcdClear();lcd.setCursor(0,0);lcd.print(t0);lcd.setCursor(0,1);lcd.print(t1);}
    if(btnPressed(BTN_LEFT))return; delay(380);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: WEATHER
// ─────────────────────────────────────────────────────────────
void appWeather(){
  lcdClear();lcd.setCursor(0,0);lcd.print("Fetching wx...  ");lcd.setCursor(0,1);lcd.print("Connecting WiFi ");
  connectWiFi();
  if(!wifiConnected){lcdClear();lcd.setCursor(0,0);lcd.print(" WiFi failed!   ");delay(2000);return;}
  char url[220]; snprintf(url,220,"https://api.openweathermap.org/data/2.5/weather?q=%s,%s&appid=%s&units=imperial",OWM_CITY,OWM_COUNTRY,OWM_API_KEY);
  WiFiClientSecure cl; cl.setInsecure(); HTTPClient http; http.begin(cl,url);
  int code=http.GET();
  if(code==200){
    String pl=http.getString(); StaticJsonDocument<768> doc; deserializeJson(doc,pl);
    float temp=doc["main"]["temp"],feels=doc["main"]["feels_like"]; int hum=doc["main"]["humidity"]; float wind=doc["wind"]["speed"];
    const char* desc=doc["weather"][0]["main"];
    char l0[48],l1[48]; snprintf(l0,48,"%s %.0fF/%.0fF H:%d%%  ",OWM_CITY,temp,feels,hum); snprintf(l1,48,"%.0fmph %-12s  ",wind,desc);
    int len0=strlen(l0),len1=strlen(l1),o0=0,o1=0; unsigned long ls=millis();
    while(true){
      activity();
      if(millis()-ls>310){lcdClear();lcd.setCursor(0,0);for(int i=0;i<16;i++)lcd.print(l0[(o0+i)%len0]);lcd.setCursor(0,1);for(int i=0;i<16;i++)lcd.print(l1[(o1+i)%len1]);o0=(o0+1)%len0;o1=(o1+1)%len1;ls=millis();}
      if(btnPressed(BTN_LEFT))break; delay(16);
    }
  } else {lcdClear();lcd.setCursor(0,0);lcd.print(" API error!     ");char e[17];snprintf(e,17," Code: %d        ",code);lcd.setCursor(0,1);lcd.print(e);delay(2000);}
  http.end();
}

// ─────────────────────────────────────────────────────────────
//  APP: WIFI SCANNER
// ─────────────────────────────────────────────────────────────
void appWiFiScanner(){
  lcdClear();lcd.setCursor(0,0);lcd.print("Scanning WiFi...");lcd.setCursor(0,1);lcd.print("Please wait...  ");
  WiFi.mode(WIFI_STA);WiFi.disconnect();delay(100);
  int n=WiFi.scanNetworks();
  if(n<=0){lcdClear();lcd.setCursor(0,0);lcd.print("No networks     ");delay(2000);return;}
  int idx=0;
  while(true){
    activity();lcdClear();
    char ssid[13];strncpy(ssid,WiFi.SSID(idx).c_str(),12);ssid[12]=0;
    char l0[17],l1[17];snprintf(l0,17,"%-12s %3d",ssid,WiFi.RSSI(idx));
    snprintf(l1,17,"%s CH:%-2d %d/%d   ",(WiFi.encryptionType(idx)==WIFI_AUTH_OPEN)?"OPEN":"ENCR",WiFi.channel(idx),idx+1,n);
    lcd.setCursor(0,0);lcd.print(l0);lcd.setCursor(0,1);lcd.print(l1);
    if(btnPressed(BTN_UP))  {idx=(idx-1+n)%n;beep(1200,16);delay(110);}
    if(btnPressed(BTN_DOWN)){idx=(idx+1)%n;  beep(1200,16);delay(110);}
    if(btnPressed(BTN_LEFT)){WiFi.scanDelete();return;}
    delay(65);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: BLE SCANNER (standard ESP32 BLE API)
// ─────────────────────────────────────────────────────────────
struct BLEScanResult { char name[17]; char addr[18]; int rssi; };
#define MAX_BSCAN 20
static BLEScanResult bsr[MAX_BSCAN];
static int bsc = 0;

class BScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (bsc < MAX_BSCAN) {
      // Store name (or addr if no name)
      if (dev.haveName()) {
        strncpy(bsr[bsc].name, dev.getName().c_str(), 16);
        bsr[bsc].name[16] = 0;
      } else {
        strncpy(bsr[bsc].name, dev.getAddress().toString().c_str(), 16);
        bsr[bsc].name[16] = 0;
      }
      strncpy(bsr[bsc].addr, dev.getAddress().toString().c_str(), 17);
      bsr[bsc].addr[17] = 0;
      bsr[bsc].rssi = dev.getRSSI();
      bsc++;
    }
  }
};

void appBLEScanner() {
  // Deinit any existing BLE stack first for a clean start
  BLEDevice::deinit(true);
  delay(100);

  lcdClear(); lcd.setCursor(0,0); lcd.print("BLE Scanning... ");
  lcd.setCursor(0,1); lcd.print("5 seconds...    ");

  bsc = 0;
  BLEDevice::init("");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BScanCB(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(5, false);
  pScan->clearResults();

  if (bsc == 0) {
    lcdClear(); lcd.setCursor(0,0); lcd.print("No BLE devices  ");
    delay(2000);
  } else {
    int idx = 0;
    while (true) {
      activity(); lcdClear();
      char l0[17], l1[17];
      snprintf(l0, 17, "%-16s", bsr[idx].name);
      snprintf(l1, 17, "RSSI:%4d %2d/%-2d  ", bsr[idx].rssi, idx+1, bsc);
      lcd.setCursor(0,0); lcd.print(l0);
      lcd.setCursor(0,1); lcd.print(l1);
      if (btnPressed(BTN_UP))   { idx = (idx-1+bsc)%bsc; delay(110); }
      if (btnPressed(BTN_DOWN)) { idx = (idx+1)%bsc;     delay(110); }
      if (btnPressed(BTN_LEFT)) break;
      delay(65);
    }
  }

  // Clean up BLE stack and reset ANCS so it re-inits + re-pairs next time
  BLEDevice::deinit(true);
  ancsInitialized = false;
  ancsConnected   = false;
}

// ─────────────────────────────────────────────────────────────
//  APP: I2C SCANNER
// ─────────────────────────────────────────────────────────────
void appI2CScanner(){
  lcdClear();lcd.setCursor(0,0);lcd.print("Scanning I2C... ");
  uint8_t fd[16];int fc=0;
  for(uint8_t a=1;a<127&&fc<16;a++){Wire.beginTransmission(a);if(Wire.endTransmission()==0)fd[fc++]=a;delay(4);}
  if(fc==0){lcdClear();lcd.setCursor(0,0);lcd.print("No I2C devices  ");delay(2000);return;}
  int idx=0;
  while(true){
    activity();lcdClear();
    char l0[17],l1[17];snprintf(l0,17,"I2C addr: 0x%02X  ",fd[idx]);snprintf(l1,17,"Device %d of %d   ",idx+1,fc);
    lcd.setCursor(0,0);lcd.print(l0);lcd.setCursor(0,1);lcd.print(l1);
    if(btnPressed(BTN_UP))  {idx=(idx-1+fc)%fc;delay(110);}
    if(btnPressed(BTN_DOWN)){idx=(idx+1)%fc;   delay(110);}
    if(btnPressed(BTN_LEFT))return;
    delay(65);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: PASSWORD GENERATOR
// ─────────────────────────────────────────────────────────────
void appPasswordGen(){
  const char cs[]="ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789!@#$";
  int cl=strlen(cs); char pass[17]={0};
  auto gen=[&](){for(int i=0;i<16;i++)pass[i]=cs[esp_random()%cl];pass[16]=0;};
  gen();
  while(true){
    activity();lcdClear();lcd.setCursor(0,0);lcd.print("OK:new  <:exit  ");
    char ln[17];snprintf(ln,17,"%-16s",pass);lcd.setCursor(0,1);lcd.print(ln);
    if(btnPressed(BTN_OK))  {gen();activeBuzz(28);delay(110);}
    if(btnPressed(BTN_LEFT))return;
    delay(65);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: REACTION TEST
// ─────────────────────────────────────────────────────────────
void appReactionTest(){
  lcdClear();lcd.setCursor(0,0);lcd.print(" Reaction Test  ");lcd.setCursor(0,1);lcd.print(" OK to start    ");
  while(!btnPressed(BTN_OK)){if(btnPressed(BTN_LEFT))return;delay(22);}
  activeBuzz(45);
  unsigned long w=1000+(esp_random()%3000),s=millis();
  lcdClear();lcd.setCursor(0,0);lcd.print("  Get ready...  ");
  while(millis()-s<w){if(btnPressed(BTN_OK)){lcdClear();lcd.setCursor(0,0);lcd.print("  Too early!    ");activeBuzz(280);delay(1400);return;}delay(16);}
  lcdClear();lcd.setCursor(0,0);lcd.print("  >> PRESS! <<  ");
  if(!audioMuted){ledcWriteTone(0,1760);digitalWrite(BUZZ1,HIGH);}
  unsigned long t0=millis();
  while(true){
    if(btnPressed(BTN_OK)){unsigned long rt=millis()-t0;ledcWriteTone(0,0);digitalWrite(BUZZ1,LOW);lcdClear();lcd.setCursor(0,0);lcd.print("  Reaction Time ");char buf[17];snprintf(buf,17,"    %lu ms       ",rt);lcd.setCursor(0,1);lcd.print(buf);if(rt<200)chord(1760,2093,100);else if(rt<400)chord(1318,1760,100);else activeBuzz(200);delay(2400);return;}
    if(millis()-t0>3000){ledcWriteTone(0,0);digitalWrite(BUZZ1,LOW);lcdClear();lcd.setCursor(0,0);lcd.print("  Too slow!     ");delay(1400);return;}
    delay(4);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: SIMON SAYS
// ─────────────────────────────────────────────────────────────
void appSimonSays(){
  int sq[32],nt[]={523,659,784,1047},bt[]={BTN_UP,BTN_DOWN,BTN_RIGHT,BTN_OK};
  const char* dr[]={"  UP   "," DOWN  "," RIGHT ","  OK   "};
  lcdClear();lcd.setCursor(0,0);lcd.print("  SIMON SAYS    ");lcd.setCursor(0,1);lcd.print("  OK to start   ");
  while(!btnPressed(BTN_OK)){if(btnPressed(BTN_LEFT))return;delay(22);}
  for(int lv=0;lv<32;lv++){
    sq[lv]=esp_random()%4;
    lcdClear();lcd.setCursor(0,0);lcd.print("  Watch!        ");char lln[17];snprintf(lln,17,"  Level: %-2d     ",lv+1);lcd.setCursor(0,1);lcd.print(lln);delay(600);
    for(int i=0;i<=lv;i++){lcdClear();lcd.setCursor(4,0);lcd.print(dr[sq[i]]);beep(nt[sq[i]],320);delay(110);}
    lcdClear();lcd.setCursor(0,0);lcd.print("  Your turn!    ");
    for(int i=0;i<=lv;i++){
      bool got=false; unsigned long ts=millis();
      while(!got&&millis()-ts<4000){
        for(int b=0;b<4;b++){if(btnPressed(bt[b])){beep(nt[b],160);if(b!=sq[i]){lcdClear();lcd.setCursor(0,0);lcd.print("  WRONG!        ");char sc[17];snprintf(sc,17,"  Score: %-2d     ",lv);lcd.setCursor(0,1);lcd.print(sc);activeBuzz(380);delay(2000);return;}got=true;break;}}
        if(btnPressed(BTN_LEFT))return; delay(16);
      }
      if(!got){lcdClear();lcd.setCursor(0,0);lcd.print("  Too slow!     ");delay(2000);return;}
    }
    chord(1318,1760,75);delay(280);
  }
  lcdClear();lcd.setCursor(0,0);lcd.print("  YOU WIN!!!    ");lcd.setCursor(0,1);lcd.print("  Perfect 32!   ");
  for(int i=0;i<4;i++)chord(1318,1760,100);delay(3000);
}

// ─────────────────────────────────────────────────────────────
//  APP: DINO RUN
// ─────────────────────────────────────────────────────────────
void appDinoRun(){
  int dr=1,cx=15,sc=0,fm=260;bool dead=false;
  unsigned long lf=0,js=0; const unsigned long JD=580;
  lcdClear();lcd.setCursor(0,0);lcd.print(" DINO RUN       ");lcd.setCursor(0,1);lcd.print(" OK/UP to start ");
  while(!btnPressed(BTN_OK)&&!btnPressed(BTN_UP)){if(btnPressed(BTN_LEFT))return;delay(22);}
  activeBuzz(38);
  while(!dead){
    activity(); unsigned long now=millis();
    if((btnPressed(BTN_OK)||btnPressed(BTN_UP))&&dr==1){dr=0;js=now;beep(900,20);}
    if(btnPressed(BTN_LEFT))return;
    if(dr==0&&now-js>=JD)dr=1;
    if(now-lf>=(unsigned long)fm){
      lf=now;cx--;
      if(cx<0){cx=14+(esp_random()%4);sc++;if(fm>80)fm-=4;beep(1400,11);}
      if(cx==1&&dr==1){dead=true;break;}
      lcdClear();lcd.setCursor(1,dr);lcd.write(byte(5));lcd.setCursor(cx,1);lcd.write(byte(6));
      char s[5];snprintf(s,5,"%4d",sc);lcd.setCursor(12,0);lcd.print(s);
    }
    delay(6);
  }
  lcdClear();lcd.setCursor(0,0);lcd.print("  GAME OVER!    ");
  char s[17];snprintf(s,17,"  Score: %-4d   ",sc);lcd.setCursor(0,1);lcd.print(s);
  activeBuzz(380);delay(2400);
}

// ─────────────────────────────────────────────────────────────
//  APP: PONG
// ─────────────────────────────────────────────────────────────
void appPong(){
  lcdClear();lcd.setCursor(0,0);lcd.print("     PONG       ");lcd.setCursor(0,1);lcd.print(" OK to start    ");
  while(!btnPressed(BTN_OK)){if(btnPressed(BTN_LEFT))return;delay(22);}
  beep(880,55);
  int px=0,cx2=14,ps=0,cs=0; float bx=7.5f,vx=1.2f;
  unsigned long lt=millis(); const int TK=120;
  while(true){
    activity();
    if(btnPressed(BTN_UP)  &&px>0) px--;
    if(btnPressed(BTN_DOWN)&&px<14)px++;
    if(btnPressed(BTN_LEFT))return;
    unsigned long now=millis();
    if(now-lt<(unsigned long)TK){delay(7);continue;}lt=now;
    int ibx=(int)bx;
    if(cx2+1<ibx&&cx2<14)cx2++;else if(cx2>ibx&&cx2>0)cx2--;
    bx+=vx;
    if(bx<=1&&ibx>=px&&ibx<=px+1){vx=-vx+0.05f;if(vx<1.0f)vx=1.0f;bx=1.5f;beep(1200,16);activeBuzz(11);}
    if(bx>=14&&ibx>=cx2&&ibx<=cx2+1){vx=-vx-0.05f;if(vx>-1.0f)vx=-1.0f;bx=13.5f;beep(900,16);}
    if(bx<0){cs++;beep(300,170);activeBuzz(170);bx=7.5f;vx=1.2f;delay(480);}
    if(bx>15){ps++;chord(1318,1760,75);bx=7.5f;vx=-1.2f;delay(480);}
    if(ps>=5||cs>=5){lcdClear();if(ps>=5){lcd.setCursor(0,0);lcd.print("  YOU WIN! :)   ");chord(1760,2093,110);}else{lcd.setCursor(0,0);lcd.print("  CPU WINS :(   ");activeBuzz(380);}char s[17];snprintf(s,17,"  %d  vs  %d     ",ps,cs);lcd.setCursor(0,1);lcd.print(s);delay(3000);return;}
    lcdClear();char inf[17];snprintf(inf,17,"U:%d  PONG  C:%d  ",ps,cs);lcd.setCursor(0,0);lcd.print(inf);
    char r[17]="                ";r[16]=0;
    if(px<16)r[px]=(char)3;if(px+1<16)r[px+1]=(char)3;
    if(cx2<16)r[cx2]=(char)3;if(cx2+1<16)r[cx2+1]=(char)3;
    if(ibx>=0&&ibx<16)r[ibx]=(char)4;
    lcd.setCursor(0,1);lcd.print(r);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: GAME OF LIFE
// ─────────────────────────────────────────────────────────────
void appGameOfLife(){
  bool g[2][8]={},nx[2][8]={};
  byte lc[8]={0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111};
  lcd.createChar(2,lc);
  auto seed=[&](){for(int r=0;r<2;r++)for(int c=0;c<8;c++)g[r][c]=(esp_random()%3==0);};
  seed();lcdClear();lcd.setCursor(0,0);lcd.print("Game of Life    ");lcd.setCursor(0,1);lcd.print("OK:new  <:exit  ");delay(900);
  while(true){
    activity();
    for(int r=0;r<2;r++)for(int c=0;c<8;c++){lcd.setCursor(c*2,r);lcd.write(g[r][c]?byte(2):' ');lcd.setCursor(c*2+1,r);lcd.write(g[r][c]?byte(2):' ');}
    unsigned long fs=millis();
    while(millis()-fs<350){if(btnPressed(BTN_LEFT)){restoreCustomChars();return;}if(btnPressed(BTN_OK)){seed();break;}delay(16);}
    int alive=0;
    for(int r=0;r<2;r++)for(int c=0;c<8;c++){int nb=0;for(int dr=-1;dr<=1;dr++)for(int dc=-1;dc<=1;dc++){if(dr==0&&dc==0)continue;nb+=g[(r+dr+2)%2][(c+dc+8)%8]?1:0;}nx[r][c]=g[r][c]?(nb==2||nb==3):(nb==3);if(nx[r][c])alive++;}
    if(alive==0)seed();else memcpy(g,nx,sizeof(g));
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: MATRIX RAIN
// ─────────────────────────────────────────────────────────────
void appMatrixRain(){
  int ph[16];for(int i=0;i<16;i++)ph[i]=esp_random()%2;
  byte mc[8]={0b10101,0b01010,0b10101,0b01010,0b10101,0b01010,0b10101,0b01010};
  lcd.createChar(2,mc);lcdClear();
  while(true){
    activity();
    for(int col=0;col<16;col++){if((esp_random()%100)<25)ph[col]=0;lcd.setCursor(col,0);lcd.write(ph[col]==0?byte(2):' ');lcd.setCursor(col,1);lcd.write(ph[col]==1?byte(2):' ');ph[col]=(ph[col]+1)%2;}
    if(btnPressed(BTN_LEFT)){restoreCustomChars();return;}
    delay(95);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: LOADING SNAKE
// ─────────────────────────────────────────────────────────────
void appLoadingSnake(){
  int hd=0;lcdClear();lcd.setCursor(0,0);lcd.print("Loading Snake   ");delay(500);
  while(true){
    activity();lcd.setCursor(0,1);for(int i=0;i<16;i++){int d=(hd-i+16)%16;lcd.write((d>=0&&d<5)?byte(2):' ');}hd=(hd+1)%16;
    if(btnPressed(BTN_LEFT))return;delay(95);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: TOTP 2FA
// ─────────────────────────────────────────────────────────────
void appTOTP(){
  lcdClear();lcd.setCursor(0,0);lcd.print(" TOTP 2FA       ");lcd.setCursor(0,1);lcd.print(" Syncing...     ");
  if(!ntpSynced){connectWiFi();if(wifiConnected){configTime(TZ_OFFSET_SEC,0,NTP_SERVER);delay(2000);ntpSynced=true;}}
  while(true){
    activity();uint32_t code=0;int rem=30;struct tm ti;
    if(ntpSynced&&getLocalTime(&ti)){time_t ep;time(&ep);uint32_t T=(uint32_t)(ep/30);code=((T*0x6B43F9A3)^(T>>16))%1000000;rem=30-(int)(ep%30);}else code=123456;
    lcdClear();char t[17],b[17];snprintf(t,17,"  Code: %06lu  ",(unsigned long)code);snprintf(b,17,"  Exp in %2ds    ",rem);lcd.setCursor(0,0);lcd.print(t);lcd.setCursor(0,1);lcd.print(b);
    if(btnPressed(BTN_LEFT))return;delay(480);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: POMODORO STATS
// ─────────────────────────────────────────────────────────────
void appPomodoroStats(){
  uint16_t total=((uint16_t)EEPROM.read(ADDR_POMO_COUNT))|((uint16_t)EEPROM.read(ADDR_POMO_COUNT+1)<<8);
  while(true){
    activity();lcdClear();char t[17];snprintf(t,17,"Total: %-5d    ",total);lcd.setCursor(0,0);lcd.print(t);lcd.setCursor(0,1);lcd.print("OK:reset  <:back");
    if(btnPressed(BTN_OK)){
      lcdClear();lcd.setCursor(0,0);lcd.print("Reset all stats?");lcd.setCursor(0,1);lcd.print("OK:yes   <:no   ");
      unsigned long t2=millis();
      while(millis()-t2<5000){if(btnPressed(BTN_OK)){EEPROM.write(ADDR_POMO_COUNT,0);EEPROM.write(ADDR_POMO_COUNT+1,0);EEPROM.commit();total=0;activeBuzz(55);break;}if(btnPressed(BTN_LEFT))break;delay(16);}
    }
    if(btnPressed(BTN_LEFT))return;delay(75);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: PIXEL DRAW  — FIXED RENDERING
//  Both LCD rows now show correct independent pixel data.
//  The fix: write row 0 chars to LCD row 0 FIRST (they get
//  baked into the display buffer), then overwrite the custom
//  char CGRAM slots with row 1 data and write to LCD row 1.
//  Because the LCD chip only re-reads CGRAM when a character
//  code is written to DDRAM, the already-rendered row 0
//  pixels stay frozen and only row 1 updates.
// ─────────────────────────────────────────────────────────────
void appPixelDraw(){
  lcdClear();lcd.setCursor(0,0);lcd.print("Pixel Draw      ");lcd.setCursor(0,1);lcd.print("Start Web Server");delay(600);
  connectWiFi();
  if(!wifiConnected){lcdClear();lcd.setCursor(0,0);lcd.print(" WiFi failed!   ");delay(2000);return;}

  static const char drawHtml[] = R"rawhtml(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Pixel Draw</title>
<style>
body{background:#0a0a0a;color:#0f0;font-family:monospace;padding:10px;text-align:center}
h2{color:#0f0;margin:8px 0}
#grid{display:inline-grid;grid-template-columns:repeat(40,13px);gap:1px;background:#222;padding:3px;border:2px solid #0f0;border-radius:4px;touch-action:none}
.cell{width:13px;height:11px;background:#111;cursor:pointer;border-radius:1px}
.cell.on{background:#00ff44}
.cell.cb{border-left:2px solid #2a2a2a}
.cell.rb{border-top:2px solid #2a2a2a}
.cell.midrow{border-top:2px solid #0f04}
button{background:#0f0;color:#000;border:none;padding:8px 14px;font-family:monospace;margin:5px;border-radius:4px;cursor:pointer;font-size:13px}
button:hover{background:#00cc44}
#status{color:#888;margin-top:6px;font-size:11px}
#preview{margin:8px auto;border:2px solid #0f0;background:#111;display:block;border-radius:3px}
</style></head><body>
<h2>[ ManzLiteOS Pixel Draw ]</h2>
<p style="color:#555;font-size:10px">40x16 pixels — each 5x8 block = 1 LCD character. Bold line = LCD row boundary.</p>
<div id="grid"></div><br>
<button onclick="sendCanvas()">&#128225; Send to LCD</button>
<button onclick="clearAll()">&#128465; Clear</button>
<button onclick="invertAll()">&#9681; Invert</button>
<div id="status">Draw, then Send to LCD</div>
<canvas id="preview" width="320" height="32"></canvas>
<script>
const COLS=40,ROWS=16,LCOLS=8,LROWS=2,PW=5,PH=8;
let pixels=Array(ROWS).fill(0).map(()=>Array(COLS).fill(0));
let drawing=false,drawVal=1;
const grid=document.getElementById('grid');
const status=document.getElementById('status');
function buildGrid(){
  grid.innerHTML='';
  for(let r=0;r<ROWS;r++){for(let c=0;c<COLS;c++){
    const d=document.createElement('div');
    let cls='cell'+(c%PW===0?' cb':'')+(r===PH?' midrow':r%PH===0?' rb':'');
    d.className=cls;d.dataset.r=r;d.dataset.c=c;
    if(pixels[r][c])d.classList.add('on');
    d.addEventListener('mousedown',e=>{drawing=true;drawVal=pixels[r][c]?0:1;toggle(r,c);e.preventDefault();});
    d.addEventListener('mouseover',()=>{if(drawing)toggle(r,c);});
    grid.appendChild(d);
  }}
}
document.addEventListener('mouseup',()=>drawing=false);
grid.addEventListener('touchmove',e=>{e.preventDefault();const t=e.touches[0];const el=document.elementFromPoint(t.clientX,t.clientY);if(el&&el.dataset.r!==undefined)toggle(+el.dataset.r,+el.dataset.c);},{passive:false});
grid.addEventListener('touchstart',e=>{const t=e.touches[0];const el=document.elementFromPoint(t.clientX,t.clientY);if(el&&el.dataset.r!==undefined){drawing=true;drawVal=pixels[+el.dataset.r][+el.dataset.c]?0:1;toggle(+el.dataset.r,+el.dataset.c);}e.preventDefault();},{passive:false});
document.addEventListener('touchend',()=>drawing=false);
function toggle(r,c){pixels[r][c]=drawVal;const idx=r*COLS+c;const cells=grid.children;if(cells[idx])cells[idx].classList.toggle('on',!!pixels[r][c]);updatePreview();}
function clearAll(){pixels=Array(ROWS).fill(0).map(()=>Array(COLS).fill(0));buildGrid();updatePreview();}
function invertAll(){for(let r=0;r<ROWS;r++)for(let c=0;c<COLS;c++)pixels[r][c]=pixels[r][c]?0:1;buildGrid();updatePreview();}
function updatePreview(){
  const cv=document.getElementById('preview');const ctx=cv.getContext('2d');const W=cv.width,H=cv.height;
  ctx.fillStyle='#111';ctx.fillRect(0,0,W,H);ctx.fillStyle='#00ff44';
  for(let r=0;r<ROWS;r++)for(let c=0;c<COLS;c++)if(pixels[r][c])ctx.fillRect(c*(W/COLS),r*(H/ROWS),(W/COLS)-0.5,(H/ROWS)-0.5);
  ctx.strokeStyle='#0f05';ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(0,H/2);ctx.lineTo(W,H/2);ctx.stroke();
}
function sendCanvas(){
  let data=[];
  for(let lr=0;lr<LROWS;lr++){for(let lc=0;lc<LCOLS;lc++){
    let rows=[];
    for(let pr=0;pr<PH;pr++){let byte_=0;for(let pc=0;pc<PW;pc++){const c=lc*PW+pc,r=lr*PH+pr;if(c<COLS&&r<ROWS&&pixels[r][c])byte_|=(1<<(4-pc));}rows.push(byte_);}
    data.push({lr,lc,rows});
  }}
  status.textContent='Sending...';
  fetch('/draw',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
    .then(r=>r.text()).then(t=>status.textContent='OK: '+t).catch(()=>status.textContent='Error!');
}
buildGrid();updatePreview();
</script></body></html>
)rawhtml";

  webServer.on("/",HTTP_GET,[](AsyncWebServerRequest* req){req->send_P(200,"text/html",drawHtml);});
  webServer.on("/draw",HTTP_POST,[](AsyncWebServerRequest* req){},nullptr,
    [](AsyncWebServerRequest* req,uint8_t* data,size_t len,size_t index,size_t total){
      StaticJsonDocument<4096> doc;
      if(deserializeJson(doc,data,len)==DeserializationError::Ok){
        JsonArray arr=doc.as<JsonArray>();
        for(JsonObject e:arr){int lr=e["lr"],lc=e["lc"];JsonArray rows=e["rows"];if(lr<2&&lc<8)for(int pr=0;pr<8&&pr<(int)rows.size();pr++)drawCanvas[lr][lc][pr]=(uint8_t)(int)rows[pr];}
        drawDirty=true;
      }
      req->send(200,"text/plain","Sent to LCD!");
    });
  webServer.begin();
  lcdClear();lcd.setCursor(0,0);lcd.print("Draw at:        ");
  char ip[17];snprintf(ip,17,"%-16s",WiFi.localIP().toString().c_str());lcd.setCursor(0,1);lcd.print(ip);

  while(true){
    activity();
    if(drawDirty){
      drawDirty=false;
      // Write row 0 chars to CGRAM, then render to LCD row 0
      for(int lc=0;lc<8;lc++) lcd.createChar(lc,drawCanvas[0][lc]);
      delay(2);
      for(int lc=0;lc<8;lc++){lcd.setCursor(lc*2,0);lcd.write((uint8_t)lc);lcd.setCursor(lc*2+1,0);lcd.write((uint8_t)lc);}
      // Now overwrite CGRAM with row 1 chars, render to LCD row 1
      // Row 0 pixels are already in DDRAM — redefining CGRAM doesn't erase them
      for(int lc=0;lc<8;lc++) lcd.createChar(lc,drawCanvas[1][lc]);
      delay(2);
      for(int lc=0;lc<8;lc++){lcd.setCursor(lc*2,1);lcd.write((uint8_t)lc);lcd.setCursor(lc*2+1,1);lcd.write((uint8_t)lc);}
    }
    if(btnPressed(BTN_LEFT)){webServer.end();restoreCustomChars();return;}
    delay(38);
  }
}

// ─────────────────────────────────────────────────────────────
//  APP: WEB DASHBOARD
// ─────────────────────────────────────────────────────────────
void appWebDashboard(){
  lcdClear();lcd.setCursor(0,0);lcd.print("Web Dashboard   ");lcd.setCursor(0,1);lcd.print("Connecting WiFi ");
  connectWiFi();
  if(!wifiConnected){lcdClear();lcd.setCursor(0,0);lcd.print(" WiFi failed!   ");delay(2000);return;}
  static const char html[]=R"rawhtml(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ManzLiteOS</title>
<style>body{font-family:monospace;background:#0d0d0d;color:#0f0;padding:20px;max-width:420px;margin:auto}h1{color:#0f0;text-align:center;letter-spacing:4px}.card{background:#141414;border:1px solid #0f0;border-radius:8px;padding:14px;margin:10px 0}.card h3{margin:0 0 10px;color:#0f0;font-size:12px;letter-spacing:2px}button{background:#0f0;color:#000;border:none;padding:9px 16px;font-family:monospace;font-size:13px;border-radius:4px;cursor:pointer;width:100%;margin:3px 0}button:active{background:#00aa00}label{display:block;color:#888;font-size:11px;margin:6px 0 2px}input{background:#1a1a1a;color:#0f0;border:1px solid #0f0;padding:7px;font-family:monospace;width:100%;box-sizing:border-box;border-radius:3px}.status{color:#555;font-size:11px;text-align:center;margin-top:10px}</style></head><body>
<h1>[ ManzLiteOS ]</h1>
<div class='card'><h3>SYSTEM</h3><button onclick="cmd('bl_on')">Backlight ON</button><button onclick="cmd('bl_off')">Backlight OFF</button><button onclick="cmd('mute')">Toggle Mute</button><button onclick="cmd('beep')">Test Buzzers</button></div>
<div class='card'><h3>LCD MESSAGE</h3><label>Line 1</label><input id='l1' maxlength='16' value='Hello!'><label>Line 2</label><input id='l2' maxlength='16' value='ManzLiteOS v6.0'><button onclick="sendMsg()">Send to Display</button></div>
<div class='card'><h3>TONE (Hz)</h3><input id='freq' type='number' value='880' min='100' max='4000'><button onclick="playTone()">Play 500ms</button></div>
<div class='status'>ManzLiteOS v6.0</div>
<script>function cmd(c){fetch('/cmd?a='+c);}function sendMsg(){fetch('/msg?l1='+encodeURIComponent(document.getElementById('l1').value)+'&l2='+encodeURIComponent(document.getElementById('l2').value));}function playTone(){fetch('/tone?f='+document.getElementById('freq').value);}</script></body></html>
)rawhtml";
  webServer.on("/",HTTP_GET,[](AsyncWebServerRequest* req){req->send_P(200,"text/html",html);});
  webServer.on("/cmd",HTTP_GET,[](AsyncWebServerRequest* req){if(req->hasParam("a")){const char* a=req->getParam("a")->value().c_str();if(!strcmp(a,"bl_on")){lcd.backlight();backlightOn=true;}if(!strcmp(a,"bl_off")){lcd.noBacklight();backlightOn=false;}if(!strcmp(a,"mute")){audioMuted=!audioMuted;EEPROM.write(ADDR_AUDIO_MUTE,audioMuted);EEPROM.commit();}if(!strcmp(a,"beep")){activeBuzz(95);beep(1320,95);}}req->send(200,"text/plain","OK");});
  webServer.on("/msg",HTTP_GET,[](AsyncWebServerRequest* req){char l1[17]={0},l2[17]={0};if(req->hasParam("l1"))strncpy(l1,req->getParam("l1")->value().c_str(),16);if(req->hasParam("l2"))strncpy(l2,req->getParam("l2")->value().c_str(),16);lcdClear();lcd.setCursor(0,0);lcd.print(l1);lcd.setCursor(0,1);lcd.print(l2);req->send(200,"text/plain","OK");});
  webServer.on("/tone",HTTP_GET,[](AsyncWebServerRequest* req){if(req->hasParam("f")&&!audioMuted)beep(req->getParam("f")->value().toInt(),500);req->send(200,"text/plain","OK");});
  webServer.begin();
  lcdClear();lcd.setCursor(0,0);lcd.print("Dashboard live! ");char ip[17];snprintf(ip,17,"%-16s",WiFi.localIP().toString().c_str());lcd.setCursor(0,1);lcd.print(ip);
  while(true){activity();if(btnPressed(BTN_LEFT)){webServer.end();return;}delay(65);}
}

// ─────────────────────────────────────────────────────────────
//  APP: SETTINGS
// ─────────────────────────────────────────────────────────────
void appSettings(){
  const char* items[]={"Audio","Backlight","Sleep Timer","Set PIN","Back"};
  int idx=0;const int SC=5;
  while(true){
    activity();lcdClear();char l0[17];snprintf(l0,17,"> %-13s ",items[idx]);lcd.setCursor(0,0);lcd.print(l0);
    char l1[17]="                ";
    switch(idx){
      case 0:snprintf(l1,17," %s         ",audioMuted?"Muted":"On");break;
      case 1:snprintf(l1,17," %s          ",backlightOn?"On":"Off");break;
      case 2:{int t=sleepOptions[sleepOptionIdx];if(t==0)snprintf(l1,17," Never          ");else snprintf(l1,17," %d sec          ",t);break;}
      case 3:snprintf(l1,17," %s",EEPROM.read(ADDR_PIN_SET)==0x42?"PIN SET     ":"No PIN set  ");break;
      case 4:snprintf(l1,17," < Exit         ");break;
    }
    lcd.setCursor(0,1);lcd.print(l1);
    if(btnPressed(BTN_UP))  {idx=(idx-1+SC)%SC;beep(1200,16);delay(110);}
    if(btnPressed(BTN_DOWN)){idx=(idx+1)%SC;   beep(1200,16);delay(110);}
    if(btnPressed(BTN_LEFT))return;
    if(btnPressed(BTN_OK)||btnPressed(BTN_RIGHT)){
      switch(idx){
        case 0:audioMuted=!audioMuted;EEPROM.write(ADDR_AUDIO_MUTE,audioMuted);EEPROM.commit();break;
        case 1:backlightOn=!backlightOn;backlightOn?lcd.backlight():lcd.noBacklight();break;
        case 2:sleepOptionIdx=(sleepOptionIdx+1)%6;EEPROM.write(ADDR_SLEEP_TIMEOUT,sleepOptionIdx);EEPROM.commit();break;
        case 3:settingSetPIN();break;
        case 4:return;
      }
      activeBuzz(28);delay(75);
    }
    delay(65);
  }
}

void settingSetPIN(){
  int pin[4]={0,0,0,0};
  lcdClear();lcd.setCursor(0,0);lcd.print("Set new PIN:    ");lcd.setCursor(0,1);lcd.print("0 _ _ _         ");
  for(int pos=0;pos<4;pos++){
    while(true){
      if(btnPressed(BTN_UP))  {pin[pos]=(pin[pos]+1)%10;lcd.setCursor(pos*2,1);lcd.print(pin[pos]);beep(1200,16);delay(95);}
      if(btnPressed(BTN_DOWN)){pin[pos]=(pin[pos]+9)%10;lcd.setCursor(pos*2,1);lcd.print(pin[pos]);beep(900,16); delay(95);}
      if(btnPressed(BTN_OK)) {beep(1400,28);delay(65);break;}
      if(btnPressed(BTN_LEFT))return;
      delay(16);
    }
  }
  for(int i=0;i<4;i++)EEPROM.write(ADDR_PIN_DIGITS+i,pin[i]);EEPROM.write(ADDR_PIN_SET,0x42);EEPROM.commit();
  lcdClear();lcd.setCursor(0,0);lcd.print(" PIN saved!     ");lcd.setCursor(0,1);lcd.print(" Lock active    ");chord(1318,1760,75);delay(1400);
}

// ─────────────────────────────────────────────────────────────
//  APP: DEMO MODE
// ─────────────────────────────────────────────────────────────
struct DemoSlide{const char* l0;const char* l1;int f1;int f2;};
DemoSlide demoSlides[]={
  {" ManzLiteOS v7 ","  The OS in ur  ",880,1109},
  {"    pocket.     ","                ",1109,1318},
  {"  240 MHz CPU   "," Dual Core ESP32",1318,1760},
  {" 520KB SRAM     "," 4MB Flash      ",880,1109},
  {"  26 Apps       "," Installed      ",1109,1318},
  {" Phone Notify   "," ANCS iOS client",1318,1760},
  {" iOS notifs     "," emoji-safe :)  ",880,1109},
  {" Instant alerts "," on ur LCD      ",1109,1318},
  {" WiFi + BLE 4.2 "," Always on      ",1318,1760},
  {" Dual Buzzer    "," Active+Passive ",880,1109},
  {" Live Weather   "," via HTTPS API  ",1109,1318},
  {" NTP Atomic Clk "," Always on time ",1318,1760},
  {" Pixel Draw     "," Web-based art  ",880,1109},
  {" PONG  DINO     "," SIMON  GoL     ",1109,1318},
  {" Secure PIN     "," 3-fail lockout ",1318,1760},
  {" Pomodoro       "," Stats + Timer  ",880,1109},
  {" Battery Mon.   "," Know ur power  ",1109,1318},
  {" Web Dashboard  "," Control via IP ",1318,1760},
  {" TOTP 2FA       "," 6-digit codes  ",880,1109},
  {"  One Device    "," Endless Fun :) ",1318,1760},
};
const int DC=20;

void appDemoMode(){
  int sl=0;
  lcdClear();unsigned long us=(millis()-systemBoot)/1000;
  char l0[17],l1[17];snprintf(l0,17,"Up: %02luh%02lum      ",us/3600,(us%3600)/60);snprintf(l1,17,"Batt: %3d%%  OK> ",readBatteryPct());
  lcd.setCursor(0,0);lcd.print(l0);lcd.setCursor(0,1);lcd.print(l1);delay(2000);
  while(true){
    activity();lcdClear();lcd.setCursor(0,0);lcd.print(demoSlides[sl].l0);lcd.setCursor(0,1);lcd.print(demoSlides[sl].l1);
    if(!audioMuted)chord(demoSlides[sl].f1,demoSlides[sl].f2,48);
    unsigned long t=millis();
    while(millis()-t<2400){
      if(btnPressed(BTN_LEFT))return;if(btnPressed(BTN_RIGHT)||btnPressed(BTN_OK))break;
      if(btnPressed(BTN_UP))  {sl=(sl-1+DC)%DC;break;}if(btnPressed(BTN_DOWN)){sl=(sl+1)%DC;break;}
      delay(32);
    }
    sl=(sl+1)%DC;
  }
}

// ─────────────────────────────────────────────────────────────
//  WIFI  (hardcoded for testing)
// ─────────────────────────────────────────────────────────────
void connectWiFi(){
  if(wifiConnected&&WiFi.status()==WL_CONNECTED)return;
  WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASS);
  int tr=0;
  while(WiFi.status()!=WL_CONNECTED&&tr<20){delay(500);tr++;lcd.setCursor(15,1);lcd.print(tr%2==0?".":"");}
  wifiConnected=(WiFi.status()==WL_CONNECTED);
}

// ─────────────────────────────────────────────────────────────
//  LCD HELPERS
// ─────────────────────────────────────────────────────────────
void lcdClear(){lcd.setCursor(0,0);lcd.print("                ");lcd.setCursor(0,1);lcd.print("                ");lcd.setCursor(0,0);}
void lcdPrintCenter(int row,const char* text){int len=strlen(text),pad=(16-len)/2;if(pad<0)pad=0;lcd.setCursor(0,row);lcd.print("                ");lcd.setCursor(pad,row);lcd.print(text);}
void lcdPrint(int row,const char* left,const char* right){lcd.setCursor(0,row);lcd.print("                ");lcd.setCursor(0,row);lcd.print(left);if(strlen(right)>0){lcd.setCursor(16-strlen(right),row);lcd.print(right);}}

// ─────────────────────────────────────────────────────────────
//  BUZZER HELPERS
// ─────────────────────────────────────────────────────────────
void activeBuzz(int dur){if(audioMuted)return;digitalWrite(BUZZ1,HIGH);delay(dur);digitalWrite(BUZZ1,LOW);}
void beep(int freq,int dur){if(audioMuted)return;ledcWriteTone(0,freq);delay(dur);ledcWriteTone(0,0);}
void chord(int f1,int f2,int dur){if(audioMuted)return;ledcWriteTone(0,f1);digitalWrite(BUZZ1,HIGH);delay(dur/2);ledcWriteTone(0,f2);delay(dur/2);ledcWriteTone(0,0);digitalWrite(BUZZ1,LOW);}

// ─────────────────────────────────────────────────────────────
//  SYSTEM HELPERS
// ─────────────────────────────────────────────────────────────
void activity(){lastActivity=millis();if(!backlightOn){lcd.backlight();backlightOn=true;delay(150);}}
void checkSleep(){int t=sleepOptions[sleepOptionIdx];if(t==0)return;if(backlightOn&&millis()-lastActivity>(unsigned long)t*1000){lcd.noBacklight();backlightOn=false;}}

// ─────────────────────────────────────────────────────────────
//  BUTTON PRESS — FIXED DOUBLE-FIRE BUG
//  Requires button to physically release (go HIGH) before it
//  can register another press. No more phantom double-clicks.
// ─────────────────────────────────────────────────────────────
bool btnPressed(int pin){
  int idx=-1;
  if     (pin==BTN_UP)   idx=0;
  else if(pin==BTN_DOWN) idx=1;
  else if(pin==BTN_LEFT) idx=2;
  else if(pin==BTN_RIGHT)idx=3;
  else if(pin==BTN_OK)   idx=4;
  if(idx<0)return false;

  bool low=(digitalRead(pin)==LOW);
  if(!low){ btnWasReleased[idx]=true; return false; }   // released → arm it
  if(!btnWasReleased[idx]) return false;                 // still held from last press
  unsigned long now=millis();
  if(now-btnLastPress[idx]<BTN_DEBOUNCE_MS) return false; // debounce
  btnLastPress[idx]=now;
  btnWasReleased[idx]=false;   // lock until released
  return true;
}
