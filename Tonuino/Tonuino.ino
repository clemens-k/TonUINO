/*
   _____         _____ _____ _____ _____
  |_   _|___ ___|  |  |     |   | |     |
    | | | . |   |  |  |-   -| | | |  |  |
    |_| |___|_|_|_____|_____|_|___|_____|
    TonUINO Version 2.1

    created by Thorsten Voß and licensed under GNU/GPL.
    Information and contribution at https://tonuino.de.
    
    Custom ROM by Clemens Kutz.
*/
#include <DFMiniMp3.h>
#include <EEPROM.h>
#include <JC_Button.h>
#include <MFRC522.h>
#include <SPI.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>
#include "soundfiles.hpp"

// === Feature Switches ===

// uncomment the below line to enable five button support
//#define USE_FIVEBUTTONS
//#define USE_SHUTDOWNPIN

// === Global Settings ===
static constexpr uint32_t TONUINO_COOKIE        = 0x1337B347;    // used for validation of RFID card and EEPROM content

// serial
static constexpr uint8_t SERIAL_RX_PIN = 2;
static constexpr uint8_t SERIAL_TX_PIN = 3;

// miniplayer
static constexpr byte    RST_PIN = 9;                 // Configurable, see typical pin layout above
static constexpr byte    SS_PIN = 10;                 // Configurable, see typical pin layout above
static constexpr uint8_t BUSY_PIN = 4;

// buttons
#define BUTTONPAUSE_PIN A0
#define BUTTONUP_PIN    A1
#define BUTTONDOWN_PIN  A2
#ifdef USE_FIVEBUTTONS
#define BUTTONFOUR_PIN A3
#define BUTTONFIVE_PIN A4
#endif

#define LONG_PRESS 1000

// Random Number Generator
#define OPENANALOG_PIN A7

// not used in all PCB layouts, see feature switch "USE_SHUTDOWNPIN"
static constexpr uint8_t SHUTDOWN_PIN = 7;

// === Typedefs & Enum Classes ===

enum class PlayMode: uint8_t {
  _0_None,
  _1_RandomTrackInFolder,          // Album Mode - play random single track
  _2_TracksInFolder,               // Album Mode - play all tracks
  _3_RandomsInFolder,              // Party Mode - play all tracks in random order
  _4_TrackInFolder,                // Single Mode - play specific track
  _5_AudioBook,                    // All track from folder, progress is stored
  _6_AdminCard,                    // Admin Card
  _7_FromTo_RandomTrack,           // AudioBook - play random track between from and to track
  _8_FromTo_AllTracks,             // Album Mode - play all tracks between from and to track
  _9_FromTo_RandomTracks,           // Party Mode - play all tracks in random order between from and to track
  LAST = _9_FromTo_RandomTracks
};

enum class PlayModifier: uint8_t {
    _0_None,
    _1_Sleeptimer,
    _2_FreezeDance,
    _3_Locked,
    _4_Toddler,
    _5_KindergartenMode,
    _6_RepeatSingleModifier,
    _7_FeedbackModifier,
    LAST = _7_FeedbackModifier
};

// access restriction to admin menu
enum class LockMode : uint8_t {
  NoLock = 0,
  CardLock,       // a special RFID card is used
  SequenceLock,   // 4 Buttons (Pause, Up, Down) pressed in correct order
  CalcLock,       // a small calculation has to be solved, like a + b = ? or a - b = ?
  LAST = CalcLock
};

// === Structs ===

/**
 * Each folder settings consists of 4 byte:
 *  ( folder number, mode, param, param2 )
 * 
 * Regular Cards are (1..99, 1..5, don't care, don't care)
 * Special Cards are (1..99, 7..9, fromTrack, toTrack)
 * Admin Cards have the data (0, 255, don't care, don't care)
 * Modifier Cards are (0, PlayModifier 1..7, don't care, don't care)
 * 
 * Folder settings are stored on rfid cards and also in eeprom for shortcuts.
 */
struct FolderSetting {
  uint8_t folder;         // Attention: 0 indicates admin or modifier card, otherwise folder name
  union {
    PlayMode     playmode;     
    PlayModifier playmodifier; // used when folder == 0, but might be 255 for admin card
  };
  union {
    uint8_t track;
    uint8_t from_track;     // used when playmode is 7..9
    uint8_t sleep_timeout;  // used when folder == 0 and playmodifier == Sleeptimer 
  };
  union {
    uint8_t to_track;
  };
};

// TODO: Add "Card type information" like "admin card", "playModifier" and "regular"
struct NfcTag {
  uint32_t      cookie;                // should be our TONUINO_COOKIE
  uint8_t       version;               // current version: 1
  FolderSetting nfcFolderSettings;
};



// === Namespaces ===

namespace config {

  static constexpr int  EEPROM_PROGRESS_ADDRESS = 0;
  static constexpr int  EEPROM_CONFIG_ADDRESS = 100;

  // TODO: track could actually be uint16_t, so currently progress in big folders cannot be saved :-(
  // TODO: this struct is currently not used
  struct FolderProgress {
    byte version;
    byte trackLastPlayed[100];
  };

  void updateProgress(byte folder, uint16_t trackNowPlaying);
  uint16_t readProgress(byte folder);

  // admin settings are stored in eeprom
  struct AdminSettings {
    uint32_t cookie;                     // should be our TONUINO_COOKIE
    uint8_t  version;                    // current version: 2    

    uint8_t  maxVolume;
    uint8_t  minVolume;
    uint8_t  initVolume;
    uint8_t  eq;

    bool     locked;
    long     standbyTimer;
    bool     invertVolumeButtons;

    FolderSetting shortCuts[4];

    LockMode adminMenuLocked;            
    uint8_t lockSequence[4];             // if LockMode::SequenceLock is used
  };

  void loadFromFlash(AdminSettings& settings); 
  bool migrateSettings(AdminSettings& settings);
  void resetToFactoryDefaults(AdminSettings& settings);

  void writeToFlash(const AdminSettings& settings);

  void dumpFolderSetting(const FolderSetting& setting);
  void dumpSettings(const AdminSettings& settings);
}

namespace player {
  uint16_t numFolders;
  uint16_t numTracksInFolder;
  
  uint16_t currentTrack;
  uint16_t firstTrack;
  uint16_t lastTrackFinished;
  uint8_t  queue[255];
  
  uint8_t  volume;

  bool isPlaying();
  
  #define TIMEOUT 1000
  void waitForTrackToFinish();
}

namespace rfid {
  MFRC522::MIFARE_Key key;
  // bool successRead;
  // byte sector = 1;
  byte blockAddr = 4;
  byte trailerBlock = 7;
  MFRC522::StatusCode status;
}

// === Forward Declarations ===

class Mp3Notify;

class Modifier;
class FeedbackModifier; 
class SleepTimer;
class FreezeDance;
class Locked;
class ToddlerMode;
class KindergardenMode;
class RepeatSingleModifier;

void setup();
void loop();

void initButtons();
void readButtons();
void volumeUpButton();
void volumeDownButton();
void nextButton();
void previousButton();

void adminMenu(bool fromCard = false);
uint8_t voiceMenu(int numberOfOptions, 
                  int startMessage, 
                  int messageOffset,
                  bool preview = false, 
                  int previewFromFolder = 0, 
                  int defaultValue = 0, 
                  bool exitWithLongPress = false);
bool askCode(uint8_t * code, uint8_t codeLength = 4);

void shuffleQueue();

void playFolder();
void playShortCut(uint8_t shortCut);
void nextTrack(uint16_t track);
void previousTrack();

void setStandbyTimer();
void disableStandbyTimer();
void checkStandbyAndShutdown();

void resetCard();
bool setupFolder(FolderSetting& theFolder);
void setupCard();
bool readCard(NfcTag * nfcTag);
void writeCard(NfcTag nfcTag);

long calcRandomSeed();
void dump_byte_array(byte * buffer, byte bufferSize);
bool compareBytes ( uint8_t a[], uint8_t b[], byte bufferSize = 4);

// === Globals ===

SoftwareSerial                       mySoftwareSerial(SERIAL_RX_PIN, SERIAL_TX_PIN);
DFMiniMp3<SoftwareSerial, Mp3Notify> mp3(mySoftwareSerial);
MFRC522                              mfrc522(SS_PIN, RST_PIN);
config::AdminSettings  mySettings;
NfcTag                 myCard;
FolderSetting          *myFolder = nullptr;
unsigned long          sleepAtMillis = 0;
bool                   knownCard = false;

Button pauseButton(BUTTONPAUSE_PIN);
Button upButton(BUTTONUP_PIN);
Button downButton(BUTTONDOWN_PIN);
#ifdef USE_FIVEBUTTONS
Button buttonFour(BUTTONFOUR_PIN);
Button buttonFive(BUTTONFIVE_PIN);
#endif
bool ignorePauseButton = false;
bool ignoreUpButton = false;
bool ignoreDownButton = false;
#ifdef USE_FIVEBUTTONS
bool ignoreButtonFour = false;
bool ignoreButtonFive = false;
#endif


// === Main Loop ===


// implement a notification class,
// its member methods will get called
//
class Mp3Notify {
  public:

    static void OnError(uint16_t errorCode) {
      
      
      // see DfMp3_Error for code meaning
      Serial.println(F("=== Mp3Notify::OnError() "));
      Serial.println();
      Serial.print("Com Error ");
      Serial.print(errorCode);
      switch (errorCode) {
        // from device
        case 1: Serial.println(F(" - Busy")); break;
        case 2: Serial.println(F(" - Sleeping")); break;
        case 3: Serial.println(F(" - SerialWrongStack")); break;
        case 4: Serial.println(F(" - CheckSumNotMatch")); break;
        case 5: Serial.println(F(" - FileIndexOut")); break;
        case 6: Serial.println(F(" - FileMismatch")); break;
        case 7: Serial.println(F(" - Advertise")); break;
        // from library v1.0.7
        case 0x81: Serial.println(F(" - RxTimeOut")); break;
        case 0x82: Serial.println(F(" - PacketSize")); break;
        case 0x83: Serial.println(F(" - PacketHeader")); break;
        case 0x84: Serial.println(F(" - PacketChecksum")); break;
        case 0xFF: Serial.println(F(" - General")); break;
        default: Serial.println();
      }
      _lastError = errorCode;
    }
    static int32_t lastError() {
      return _lastError;
    }
    
    static int32_t popLastError() {
      int32_t returnValue;
      returnValue = _lastError;
      _lastError = 0;
      return _lastError;
    }
    static void PrintlnSourceAction(DfMp3_PlaySources source, const char* action) {
      if (source & DfMp3_PlaySources_Sd) Serial.print("SD Karte ");
      if (source & DfMp3_PlaySources_Usb) Serial.print("USB ");
      if (source & DfMp3_PlaySources_Flash) Serial.print("Flash ");
      Serial.println(action);
    }
    static void OnPlayFinished(DfMp3_PlaySources source, uint16_t track) {
      Serial.println(F("=== Mp3Notify::OnPlayFinished() "));
      //      Serial.print("Track beendet");
      //      Serial.println(track);
      //      delay(100);
      nextTrack(track);
    }
    static void OnPlaySourceOnline(DfMp3_PlaySources source) {
      PrintlnSourceAction(source, "online");
    }
    static void OnPlaySourceInserted(DfMp3_PlaySources source) {
      PrintlnSourceAction(source, "bereit");
    }
    static void OnPlaySourceRemoved(DfMp3_PlaySources source) {
      PrintlnSourceAction(source, "entfernt");
    }

  private:

    static int32_t _lastError; // 0 = no error
};

int32_t Mp3Notify::_lastError = 0;



void shuffleQueue() {
  Serial.println(F("=== shuffleQueue()"));
  // Queue für die Zufallswiedergabe erstellen
  for (uint8_t x = 0; x < player::numTracksInFolder - player::firstTrack + 1; x++)
    player::queue[x] = x + player::firstTrack;
  // Rest mit 0 auffüllen
  for (uint8_t x = player::numTracksInFolder - player::firstTrack + 1; x < 255; x++)
    player::queue[x] = 0;
  // Queue mischen
  for (uint8_t i = 0; i < player::numTracksInFolder - player::firstTrack + 1; i++)
  {
    uint8_t j = random (0, player::numTracksInFolder - player::firstTrack + 1);
    uint8_t t = player::queue[i];
    player::queue[i] = player::queue[j];
    player::queue[j] = t;
  }
  /*  Serial.println(F("Queue :"));
    for (uint8_t x = 0; x < player::numTracksInFolder - player::firstTrack + 1 ; x++)
      Serial.println(player::queue[x]);
  */
}

void config::updateProgress(byte folder, uint16_t trackNowPlaying){
  if (folder == 0 or folder > 99){
    Serial.println(F("=== config::updateProgress() - Error: folder numbers cannot be greater than 99!"));
    return;
  }
  if (trackNowPlaying > 255){
    Serial.println(F("=== config::updateProgress() - Error: track numbers cannot be greater than 255!"));
    return;
  }
  EEPROM.update(EEPROM_PROGRESS_ADDRESS + folder, trackNowPlaying);
}

uint16_t config::readProgress(byte folder){
  return EEPROM.read(EEPROM_PROGRESS_ADDRESS + folder);
}

void config::writeToFlash(const AdminSettings& settings) {
  Serial.println(F("=== writeSettingsToFlash()"));
  // int address = sizeof(myFolder->folder) * 100;
  EEPROM.put(EEPROM_CONFIG_ADDRESS, settings);
}

void config::resetToFactoryDefaults(AdminSettings& settings) {
  Serial.println(F("=== resetToFactoryDefaults()"));
  settings.cookie = TONUINO_COOKIE;
  settings.version = 2;
  settings.maxVolume = 25;
  settings.minVolume = 5;
  settings.initVolume = 8;
  settings.eq = 1;
  settings.locked = false;
  settings.standbyTimer = 0;
  settings.invertVolumeButtons = true;
  settings.shortCuts[0].folder = 0;
  settings.shortCuts[1].folder = 0;
  settings.shortCuts[2].folder = 0;
  settings.shortCuts[3].folder = 0;
  settings.adminMenuLocked = LockMode::NoLock;
  // 4 times x UP button
  settings.lockSequence[0] = 2;
  settings.lockSequence[1] = 2;
  settings.lockSequence[2] = 2;
  settings.lockSequence[3] = 2;
}


/**
 * Converts settings from older settings version 1 to current admin settings version 2
 * 
 * @returns true if settings needed to be migrated
 */
bool config::migrateSettings(AdminSettings& settings) {
  if (settings.version == 1) {
    Serial.println(F("=== migrateSettings()"));
    settings.version = 2;
    settings.adminMenuLocked = LockMode::NoLock;
    settings.lockSequence[0] = 1;
    settings.lockSequence[1] = 2;
    settings.lockSequence[2] = 3;
    settings.lockSequence[3] = 4;
    return true;
  }
  return false;
}

void config::dumpFolderSetting(const FolderSetting& setting){
  Serial.print(F("Folder = "));
  Serial.print(setting.folder);
  Serial.print(F(", Playmode = "));
  Serial.println(static_cast<int>(setting.playmode)); 
}


void config::dumpSettings(const AdminSettings& settings){
  Serial.print(F("Version: "));
  Serial.println(settings.version);

  Serial.print(F("Maximal Volume: "));
  Serial.println(settings.maxVolume);

  Serial.print(F("Minimal Volume: "));
  Serial.println(settings.minVolume);

  Serial.print(F("Initial Volume: "));
  Serial.println(settings.initVolume);

  Serial.print(F("EQ: "));
  Serial.println(settings.eq);

  Serial.print(F("Locked: "));
  Serial.println(settings.locked);

  Serial.print(F("Sleep Timer: "));
  Serial.println(settings.standbyTimer);

  Serial.print(F("Inverted Volume Buttons: "));
  Serial.println(settings.invertVolumeButtons);

  Serial.print(F("Shortcuts [0 / Play]: "));
  dumpFolderSetting(settings.shortCuts[0]);

  Serial.print(F("Shortcuts [1 / Plus]: "));
  dumpFolderSetting(settings.shortCuts[1]);

  Serial.print(F("Shortcuts [2 / Minus]: "));
  dumpFolderSetting(settings.shortCuts[2]);

  Serial.print(F("Shortcuts [3 / Startup Sound]: "));
  dumpFolderSetting(settings.shortCuts[3]);

  Serial.print(F("Admin Menu locked: "));
  Serial.println(static_cast<int>(mySettings.adminMenuLocked));

  Serial.print(F("Admin Menu Pin: "));
  Serial.print(settings.lockSequence[0]);
  Serial.print(settings.lockSequence[1]);
  Serial.print(settings.lockSequence[2]);
  Serial.println(settings.lockSequence[3]);
}

void config::loadFromFlash(AdminSettings& settings) {
  Serial.println(F("=== config::loadFromFlash()"));
  // int address = sizeof(myFolder->folder) * 100;
  if ( EEPROM_CONFIG_ADDRESS + sizeof(settings) > EEPROM.length()){
    Serial.println(F("ERROR: EEPROM too small, cannot load/store settings. Load factory defaults!"));
    resetToFactoryDefaults(settings);
  }
  else {
    EEPROM.get(EEPROM_CONFIG_ADDRESS, settings);
    if (settings.cookie != TONUINO_COOKIE){
      Serial.println(F("Cookie doesn't match, need to reset settings!"));
      resetToFactoryDefaults(settings);
      writeToFlash(settings);
    }
    if (migrateSettings(settings))
      writeToFlash(settings);
  }
  dumpSettings(settings);
}

class Modifier {
  public:
    virtual void loop() {}
    virtual bool handlePause() {
      return false;
    }
    virtual bool handleNext() {
      return false;
    }
    virtual bool handlePrevious() {
      return false;
    }
    virtual bool handleNextButton() {
      return false;
    }
    virtual bool handlePreviousButton() {
      return false;
    }
    virtual bool handleVolumeUp() {
      return false;
    }
    virtual bool handleVolumeDown() {
      return false;
    }
    /**
     * @returns True when reading the card is completed
     */
    virtual bool handleRFID(NfcTag *newCard) {
      return false;
    }
    virtual uint8_t getActive() {
      return 0;
    }
    Modifier() {

    }
};

// TODO: CHange to Feedbackmodifier -> need to move downwards or forward declare class
class FeedbackModifier;
Modifier *activeModifier = NULL;

class SleepTimer: public Modifier {
  private:
    unsigned long sleepAtMillis = 0;

  public:
    void loop() override {
      if (this->sleepAtMillis != 0 && millis() > this->sleepAtMillis) {
        Serial.println(F("=== SleepTimer::loop() -> SLEEP!"));
        mp3.pause();
        setStandbyTimer();
        activeModifier = NULL;
        delete this;
      }
    }

    SleepTimer(uint8_t minutes) {
      Serial.println(F("=== SleepTimer()"));
      Serial.println(minutes);
      this->sleepAtMillis = millis() + minutes * 60000;
      //      if (player::isPlaying())
      //        mp3.playAdvertisement(302);
      //      delay(500);
    }

    uint8_t getActive() override {
      Serial.println(F("== SleepTimer::getActive()"));
      return 1;
    }
};

class FreezeDance: public Modifier {
  public:
    FreezeDance(void) {
      Serial.println(F("=== FreezeDance()"));
      if (player::isPlaying()) {
        delay(1000);
        mp3.playAdvertisement(300);
        delay(500);
      }
      setNextStopAtMillis();
    }
    
    void loop() override {
      if (this->nextStopAtMillis != 0 && millis() > this->nextStopAtMillis) {
        Serial.println(F("== FreezeDance::loop() -> FREEZE!"));
        if (player::isPlaying()) {
          mp3.playAdvertisement(301);
          delay(500);
        }
        setNextStopAtMillis();
      }
    }

    uint8_t getActive() override {
      Serial.println(F("== FreezeDance::getActive()"));
      return 2;
    }

  private:
    unsigned long nextStopAtMillis = 0;
    const uint8_t minSecondsBetweenStops = 5;
    const uint8_t maxSecondsBetweenStops = 30;

    void setNextStopAtMillis() {
      uint16_t seconds = random(this->minSecondsBetweenStops, this->maxSecondsBetweenStops + 1);
      Serial.println(F("=== FreezeDance::setNextStopAtMillis()"));
      Serial.println(seconds);
      this->nextStopAtMillis = millis() + seconds * 1000;
    }
};

class Locked: public Modifier {
  public:
    Locked(void) {
      Serial.println(F("=== Locked()"));
      //      if (player::isPlaying())
      //        mp3.playAdvertisement(303);
    }
    bool handlePause() override    {
      Serial.println(F("== Locked::handlePause() -> LOCKED!"));
      return true;
    }
    bool handleNextButton() override      {
      Serial.println(F("== Locked::handleNextButton() -> LOCKED!"));
      return true;
    }
    bool handlePreviousButton() override {
      Serial.println(F("== Locked::handlePreviousButton() -> LOCKED!"));
      return true;
    }
    bool handleVolumeUp() override  {
      Serial.println(F("== Locked::handleVolumeUp() -> LOCKED!"));
      return true;
    }
    bool handleVolumeDown() override {
      Serial.println(F("== Locked::handleVolumeDown() -> LOCKED!"));
      return true;
    }
    bool handleRFID(NfcTag *newCard) override {
      Serial.println(F("== Locked::handleRFID() -> LOCKED!"));
      return true;
    }
    uint8_t getActive() override {
      Serial.println(F("== Locked::getActive()"));
      return 3;
    }
};

class ToddlerMode: public Modifier {
  public:
    ToddlerMode(void) {
      Serial.println(F("=== ToddlerMode()"));
      //      if (player::isPlaying())
      //        mp3.playAdvertisement(304);
    }
    bool handlePause() override    {
      Serial.println(F("== ToddlerMode::handlePause() -> LOCKED!"));
      return true;
    }
    bool handleNextButton() override      {
      Serial.println(F("== ToddlerMode::handleNextButton() -> LOCKED!"));
      return true;
    }
    bool handlePreviousButton() override {
      Serial.println(F("== ToddlerMode::handlePreviousButton() -> LOCKED!"));
      return true;
    }
    bool handleVolumeUp() override  {
      Serial.println(F("== ToddlerMode::handleVolumeUp() -> LOCKED!"));
      return true;
    }
    bool handleVolumeDown() override {
      Serial.println(F("== ToddlerMode::handleVolumeDown() -> LOCKED!"));
      return true;
    }
    uint8_t getActive() override {
      Serial.println(F("== ToddlerMode::getActive()"));
      return 4;
    }
};

class KindergardenMode: public Modifier {
  private:
    NfcTag nextCard;
    bool cardQueued = false;

  public:
    KindergardenMode() {
      Serial.println(F("=== KindergardenMode()"));
      //      if (player::isPlaying())
      //        mp3.playAdvertisement(305);
      //      delay(500);
    }
    
    bool handleNext() override {
      Serial.println(F("== KindergardenMode::handleNext() -> NEXT"));
      //if (this->nextCard.cookie == TONUINO_COOKIE && this->nextCard.nfcFolderSettings.folder != 0 && this->nextCard.nfcFolderSettings.playmode != 0) {
      //myFolder = &this->nextCard.nfcFolderSettings;
      if (this->cardQueued == true) {
        this->cardQueued = false;

        myCard = nextCard;
        myFolder = &myCard.nfcFolderSettings;
        Serial.println(myFolder->folder);
        Serial.println((uint8_t) myFolder->playmode);
        playFolder();
        return true;
      }
      return false;
    }
    //    virtual bool handlePause()     {
    //      Serial.println(F("== KindergardenMode::handlePause() -> LOCKED!"));
    //      return true;
    //    }

    bool handleNextButton() override      {
      Serial.println(F("== KindergardenMode::handleNextButton() -> LOCKED!"));
      return true;
    }

    bool handlePreviousButton() override {
      Serial.println(F("== KindergardenMode::handlePreviousButton() -> LOCKED!"));
      return true;
    }

    bool handleRFID(NfcTag * newCard) override { // lot of work to do!
      Serial.println(F("== KindergardenMode::handleRFID() -> queued!"));
      this->nextCard = *newCard;
      this->cardQueued = true;
      if (!player::isPlaying()) {
        handleNext();
      }
      return true;
    }

    uint8_t getActive() override {
      Serial.println(F("== KindergardenMode::getActive()"));
      return 5;
    }
};

class RepeatSingleModifier: public Modifier {
  public:
    RepeatSingleModifier() {
      Serial.println(F("=== RepeatSingleModifier()"));
    }
    
    bool handleNext() override {
      Serial.println(F("== RepeatSingleModifier::handleNext() -> REPEAT CURRENT TRACK"));
      delay(50);
      if (player::isPlaying()) return true;
      if (myFolder->playmode == PlayMode::_3_RandomsInFolder || myFolder->playmode == PlayMode::_9_FromTo_RandomTracks) {
        mp3.playFolderTrack(myFolder->folder, player::queue[player::currentTrack - 1]);
      }
      else {
        mp3.playFolderTrack(myFolder->folder, player::currentTrack);
      }
      player::lastTrackFinished = 0;
      return true;
    }

    uint8_t getActive() override {
      Serial.println(F("== RepeatSingleModifier::getActive()"));
      return 6;
    }
};

// An modifier can also do somethings in addition to the modified action
// by returning false (not handled) at the end
// This simple FeedbackModifier will tell the volume before changing it and
// give some feedback once a RFID card is detected.
class FeedbackModifier: public Modifier {
  public:
    bool handleVolumeDown() override {
      if (player::volume > mySettings.minVolume) {
        mp3.playAdvertisement(getAdvertNumber(player::volume - 1));
      }
      else {
        mp3.playAdvertisement(getAdvertNumber(player::volume));
      }
      delay(500);
      Serial.println(F("== FeedbackModifier::handleVolumeDown()!"));
      return false;
    }

    bool handleVolumeUp() override {
      if (player::volume < mySettings.maxVolume) {
        mp3.playAdvertisement(getAdvertNumber(player::volume + 1));
      }
      else {
        mp3.playAdvertisement(getAdvertNumber(player::volume));
      }
      delay(500);
      Serial.println(F("== FeedbackModifier::handleVolumeUp()!"));
      return false;
    }

    bool handleRFID(NfcTag *newCard) override {
      Serial.println(F("== FeedbackModifier::handleRFID()"));
      return false;
    }
    
    uint8_t getActive() override {
      Serial.println(F("== RepeatSingleModifier::getActive()"));
      return (uint8_t) PlayModifier::_7_FeedbackModifier;
    }
};

// Leider kann das Modul selbst keine Queue abspielen, daher müssen wir selbst die Queue verwalten
void nextTrack(uint16_t track) {
  Serial.println(track);
  if (activeModifier != NULL)
    if (activeModifier->handleNext() == true)
      return;

  if (track == player::lastTrackFinished) {
    return;
  }
  player::lastTrackFinished = track;

  if (knownCard == false)
    // Wenn eine neue Karte angelernt wird soll das Ende eines Tracks nicht
    // verarbeitet werden
    return;

  Serial.println(F("=== nextTrack()"));

  if (myFolder->playmode == PlayMode::_1_RandomTrackInFolder || myFolder->playmode == PlayMode::_7_FromTo_RandomTrack) {
    Serial.println(F("Audio Book Mode -> don't play another track"));
    setStandbyTimer();
    //    mp3.sleep(); // Je nach Modul kommt es nicht mehr zurück aus dem Sleep!
  }
  if (myFolder->playmode == PlayMode::_2_TracksInFolder || myFolder->playmode == PlayMode::_8_FromTo_AllTracks) {
    if (player::currentTrack != player::numTracksInFolder) {
      player::currentTrack = player::currentTrack + 1;
      mp3.playFolderTrack(myFolder->folder, player::currentTrack);
      Serial.print(F("Audio Book Mode -> play next track: "));
      Serial.print(player::currentTrack);
    } else
      //      mp3.sleep();   // Je nach Modul kommt es nicht mehr zurück aus dem Sleep!
      setStandbyTimer();
    { }
  }
  if (myFolder->playmode == PlayMode::_3_RandomsInFolder || myFolder->playmode == PlayMode::_9_FromTo_RandomTracks) {
    if (player::currentTrack != player::numTracksInFolder - player::firstTrack + 1) {
      Serial.print(F("Party Mode -> play next in queue: "));
      player::currentTrack++;
    } else {
      Serial.println(F("End of queue -> start from beginning"));
      player::currentTrack = 1;
      //// Wenn am Ende der Queue neu gemischt werden soll bitte die Zeilen wieder aktivieren
      //     Serial.println(F("Ende der Queue -> mische neu"));
      //     shuffleQueue();
    }
    Serial.println(player::queue[player::currentTrack - 1]);
    mp3.playFolderTrack(myFolder->folder, player::queue[player::currentTrack - 1]);
  }

  if (myFolder->playmode == PlayMode::_4_TrackInFolder) {
    Serial.println(F("Single Track Mode -> Save power"));
    //    mp3.sleep();      // Je nach Modul kommt es nicht mehr zurück aus dem Sleep!
    setStandbyTimer();
  }
  if (myFolder->playmode == PlayMode::_5_AudioBook) {
    if (player::currentTrack != player::numTracksInFolder) {
      player::currentTrack = player::currentTrack + 1;
      Serial.print(F("Audio book Mode -> play next Track and save progress"));
      Serial.println(player::currentTrack);
      mp3.playFolderTrack(myFolder->folder, player::currentTrack);
      // Fortschritt im EEPROM abspeichern
      config::updateProgress(myFolder->folder, player::currentTrack);
    } else {
      //      mp3.sleep();  // Je nach Modul kommt es nicht mehr zurück aus dem Sleep!
      // Fortschritt zurück setzen
      config::updateProgress(myFolder->folder, 1);
      setStandbyTimer();
    }
  }
  delay(500);
}

void previousTrack() {
  Serial.println(F("=== previousTrack()"));
  /*  if (myCard.playmode == PlayMode::_1_RandomTrackInFolder || myCard.mode == PlayMode::_7_FromTo_RandomTrack) {
      Serial.println(F("Hörspielmodus ist aktiv -> Track von vorne spielen"));
      mp3.playFolderTrack(myCard.folder, player::currentTrack);
    }*/
  if (myFolder->playmode == PlayMode::_2_TracksInFolder || myFolder->playmode == PlayMode::_8_FromTo_AllTracks) {
    Serial.println(F("Albummodus ist aktiv -> vorheriger Track"));
    if (player::currentTrack != player::firstTrack) {
      player::currentTrack = player::currentTrack - 1;
    }
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }
  if (myFolder->playmode == PlayMode::_3_RandomsInFolder || myFolder->playmode == PlayMode::_9_FromTo_RandomTracks) {
    if (player::currentTrack != 1) {
      Serial.print(F("Party Modus ist aktiv -> zurück in der Qeueue "));
      player::currentTrack--;
    }
    else
    {
      Serial.print(F("Anfang der Queue -> springe ans Ende "));
      player::currentTrack = player::numTracksInFolder;
    }
    Serial.println(player::queue[player::currentTrack - 1]);
    mp3.playFolderTrack(myFolder->folder, player::queue[player::currentTrack - 1]);
  }
  if (myFolder->playmode == PlayMode::_4_TrackInFolder) {
    Serial.println(F("Einzel Modus aktiv -> Track von vorne spielen"));
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }
  if (myFolder->playmode == PlayMode::_5_AudioBook) {
    Serial.println(F("Hörbuch Modus ist aktiv -> vorheriger Track und "
                     "Fortschritt speichern"));
    if (player::currentTrack != 1) {
      player::currentTrack = player::currentTrack - 1;
    }
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
    // Fortschritt im EEPROM abspeichern
    config::updateProgress(myFolder->folder, player::currentTrack);
  }
  delay(1000);
}





/// Funktionen für den Standby Timer (z.B. über Pololu-Switch oder Mosfet)

void setStandbyTimer() {
  Serial.println(F("=== setstandbyTimer()"));
  if (mySettings.standbyTimer != 0)
    sleepAtMillis = millis() + (mySettings.standbyTimer * 60 * 1000);
  else
    sleepAtMillis = 0;
  Serial.println(sleepAtMillis);
}

void disableStandbyTimer() {
  Serial.println(F("=== disablestandby()"));
  sleepAtMillis = 0;
}

void checkStandbyAndShutdown() {
  if (sleepAtMillis != 0 && millis() > sleepAtMillis) {
    Serial.println(F("=== power off!"));
    #ifdef USE_SHUTDOWNPIN
      // enter sleep state
      digitalWrite(SHUTDOWN_PIN, HIGH);
      delay(500);
    #endif

    // http://discourse.voss.earth/t/intenso-s10000-powerbank-automatische-abschaltung-software-only/805
    // powerdown to 27mA (powerbank switches off after 30-60s)
    mfrc522.PCD_AntennaOff();
    mfrc522.PCD_SoftPowerDown();
    mp3.sleep();

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    cli();  // Disable interrupts
    sleep_mode();
  }
}

bool player::isPlaying() {
  return !digitalRead(BUSY_PIN);
}

void player::waitForTrackToFinish() {
  long currentTime = millis();
  do {
    mp3.loop();
  } while (!player::isPlaying() && millis() < currentTime + TIMEOUT);
  delay(1000);
  do {
    mp3.loop();
  } while (player::isPlaying());
}




void setup() {

  Serial.begin(115200);

  // Dieser Hinweis darf nicht entfernt werden
  Serial.println(F("\n _____         _____ _____ _____ _____"));
  Serial.println(F("|_   _|___ ___|  |  |     |   | |     |"));
  Serial.println(F("  | | | . |   |  |  |-   -| | | |  |  |"));
  Serial.println(F("  |_| |___|_|_|_____|_____|_|___|_____|\n"));
  Serial.println(F("Custom ROM by Clemens, based on TonUINO Version 2.1"));
  Serial.println(F("created by Thorsten Voß and licensed under GNU/GPL."));
  Serial.println(F("Information and contribution at https://tonuino.de.\n"));

  // Initialize Random Number Generator
  long mySeed = calcRandomSeed();
  Serial.print(F("Random seed: "));
  Serial.println(mySeed);
  randomSeed(mySeed);

  pinMode(BUSY_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // load Settings from EEPROM
  config::loadFromFlash(mySettings);

  // activate standby timer
  setStandbyTimer();

  {
    uint8_t DplayerVolume;
    bool    ledState;

    Serial.println(F("=== Initialize DPlayer \n"));
    // DFPlayer Mini initialisieren
    mp3.begin();
    
    do {
      digitalWrite(LED_BUILTIN, ledState);
      ledState = !ledState;
      
      delay(500); // initial timeout was 2 seconds, but by using 500ms and a loop we can speed it up
      DplayerVolume = mp3.getVolume();
      Serial.print(F("DplayerVolume : "));
      Serial.println(DplayerVolume);
      
      Serial.print(F("Set initial volume: "));
      player::volume = mySettings.initVolume;
      Serial.println(player::volume);
      mp3.setVolume(player::volume);
    } while (DplayerVolume != player::volume);
    
    mp3.setEq(mySettings.eq - 1);
    
    player::numFolders = mp3.getTotalFolderCount();
  }

  Serial.println(F("=== Initialize RFID \n"));
  // NFC Leser initialisieren
  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522
  mfrc522.PCD_DumpVersionToSerial(); // Show details of PCD - MFRC522 Card Reader
  for (byte i = 0; i < 6; i++) {
    rfid::key.keyByte[i] = 0xFF;
  }

  Serial.println(F("=== Initialize Buttons \n"));
  initButtons();

  #ifdef USE_SHUTDOWNPIN
    Serial.println(F("=== Initialize Shutdown Pin \n"));
    pinMode(SHUTDOWN_PIN, OUTPUT);
    digitalWrite(SHUTDOWN_PIN, LOW);
  #endif


  Serial.println(F("=== Check Factory Reset condition \n"));
  // RESET --- ALLE DREI KNÖPFE BEIM STARTEN GEDRÜCKT HALTEN -> alle EINSTELLUNGEN werden gelöscht
  if (pauseButton.isPressed() && upButton.isPressed() && downButton.isPressed()) {
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
    config::resetToFactoryDefaults(mySettings);
    config::writeToFlash(mySettings);
  }

  Serial.println(F("=== PlayShortCut(3) - Welcome Sound \n"));
  // Start Shortcut "at Startup" - e.g. Welcome Sound
  playShortCut(3);
}

void initButtons(){
  pauseButton.begin();
  upButton.begin();
  downButton.begin();
#ifdef USE_FIVEBUTTONS
  buttonFour.begin();
  buttonFive.begin();
#endif
}

void readButtons() {
  pauseButton.read();
  upButton.read();
  downButton.read();
#ifdef USE_FIVEBUTTONS
  buttonFour.read();
  buttonFive.read();
#endif
}

void readButtonsUntilReleased(){
  do {
        readButtons();
  } while (pauseButton.isPressed() || upButton.isPressed() || downButton.isPressed());
  readButtons();
}

void volumeUpButton() {
  if (activeModifier != NULL)
    if (activeModifier->handleVolumeUp() == true)
      return;

  Serial.println(F("=== volumeUp()"));
  if (player::volume < mySettings.maxVolume) {
    mp3.increaseVolume();
    player::volume++;
  }
  Serial.println(player::volume);
}

void volumeDownButton() {
  if (activeModifier != NULL)
    if (activeModifier->handleVolumeDown() == true)
      return;

  Serial.println(F("=== volumeDown()"));
  if (player::volume > mySettings.minVolume) {
    mp3.decreaseVolume();
    player::volume--;
  }
  Serial.println(player::volume);
}

void nextButton() {
  if (activeModifier != NULL)
    if (activeModifier->handleNextButton() == true)
      return;
  Serial.println(F("=== nextButton()"));
  nextTrack(random(65536));
  delay(1000);
}

void previousButton() {
  if (activeModifier != NULL)
    if (activeModifier->handlePreviousButton() == true)
      return;
  Serial.println(F("=== previousButton()"));
  previousTrack();
  delay(1000);
}

void playFolder() {
  Serial.println(F("== playFolder()")) ;
  disableStandbyTimer();
  knownCard = true;
  player::lastTrackFinished = 0;
  player::numTracksInFolder = mp3.getFolderTrackCount(myFolder->folder);
  player::firstTrack = 1;
  Serial.print(player::numTracksInFolder);
  Serial.print(F(" Dateien in Ordner "));
  Serial.println(myFolder->folder);

  // Hörspielmodus: eine zufällige Datei aus dem Ordner
  if (myFolder->playmode == PlayMode::_1_RandomTrackInFolder) {
    Serial.println(F("Hörspielmodus -> zufälligen Track wiedergeben"));
    player::currentTrack = random(1, player::numTracksInFolder + 1);
    Serial.println(player::currentTrack);
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }
  // Album Modus: kompletten Ordner spielen
  if (myFolder->playmode == PlayMode::_2_TracksInFolder) {
    Serial.println(F("Album Modus -> kompletten Ordner wiedergeben"));
    player::currentTrack = 1;
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }
  // Party Modus: einen Ordner in zufälliger Reihenfolge
  if (myFolder->playmode == PlayMode::_3_RandomsInFolder) {
    Serial.println(
      F("Party Modus -> Ordner in zufälliger Reihenfolge wiedergeben"));
    shuffleQueue();
    player::currentTrack = 1;
    mp3.playFolderTrack(myFolder->folder, player::queue[player::currentTrack - 1]);
  }
  // Einzel Modus: eine Datei aus dem Ordner abspielen
  if (myFolder->playmode == PlayMode::_4_TrackInFolder) {
    Serial.println(
      F("Einzel Modus -> eine Datei aus dem Ordner abspielen"));
    player::currentTrack = myFolder->track;
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }
  // Hörbuch Modus: kompletten Ordner spielen und Fortschritt merken
  if (myFolder->playmode == PlayMode::_5_AudioBook) {
    Serial.println(F("Hörbuch Modus -> kompletten Ordner spielen und "
                     "Fortschritt im EEPROM merken"));
    player::currentTrack = config::readProgress(myFolder->folder);
    if (player::currentTrack == 0 || player::currentTrack > player::numTracksInFolder) {
      player::currentTrack = 1;
    }
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }
  // Spezialmodus Von-Bin: Hörspiel: eine zufällige Datei aus dem Ordner
  if (myFolder->playmode == PlayMode::_7_FromTo_RandomTrack) {
    Serial.println(F("Spezialmodus Von-Bin: Hörspiel -> zufälligen Track wiedergeben"));
    Serial.print(myFolder->from_track);
    Serial.print(F(" bis "));
    Serial.println(myFolder->to_track);
    player::numTracksInFolder = myFolder->to_track;
    player::currentTrack = random(myFolder->from_track, player::numTracksInFolder + 1);
    Serial.println(player::currentTrack);
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }

  // Spezialmodus Von-Bis: Album: alle Dateien zwischen Start und Ende spielen
  if (myFolder->playmode == PlayMode::_8_FromTo_AllTracks) {
    Serial.println(F("Spezialmodus Von-Bis: Album: alle Dateien zwischen Start- und Enddatei spielen"));
    Serial.print(myFolder->from_track);
    Serial.print(F(" bis "));
    Serial.println(myFolder->to_track);
    player::numTracksInFolder = myFolder->to_track;
    player::currentTrack = myFolder->from_track;
    mp3.playFolderTrack(myFolder->folder, player::currentTrack);
  }

  // Spezialmodus Von-Bis: Party Ordner in zufälliger Reihenfolge
  if (myFolder->playmode == PlayMode::_9_FromTo_RandomTracks) {
    Serial.println(
      F("Spezialmodus Von-Bis: Party -> Ordner in zufälliger Reihenfolge wiedergeben"));
    player::firstTrack = myFolder->from_track;
    player::numTracksInFolder = myFolder->to_track;
    shuffleQueue();
    player::currentTrack = 1;
    mp3.playFolderTrack(myFolder->folder, player::queue[player::currentTrack - 1]);
  }
}

void playShortCut(uint8_t shortCut) {
  Serial.println(F("=== playShortCut()"));
  Serial.println(shortCut);
  if (mySettings.shortCuts[shortCut].folder != 0) {
    myFolder = &mySettings.shortCuts[shortCut];
    playFolder();
    disableStandbyTimer();
    delay(1000);
  }
  else
    Serial.println(F("Shortcut not configured!"));
}

void loop() {
  do {
    checkStandbyAndShutdown();
    mp3.loop(); //check serial interface to mp3 for events, might trigger MP3 Notifier class

    // Modifier : WIP!
    if (activeModifier != NULL) {
      activeModifier->loop();
    }

    // Buttons werden nun über JS_Button gehandelt, dadurch kann jede Taste
    // doppelt belegt werden
    readButtons();

    // admin menu
    if ((pauseButton.pressedFor(LONG_PRESS) || upButton.pressedFor(LONG_PRESS) || downButton.pressedFor(LONG_PRESS)) && 
        (pauseButton.isPressed() && upButton.isPressed() && downButton.isPressed()) ) {
      mp3.pause();
      do {
        readButtons();
      } while (pauseButton.isPressed() || upButton.isPressed() || downButton.isPressed());
      readButtons();
      adminMenu();
      break;
    }

    if (pauseButton.wasReleased()) {
      if (activeModifier != NULL)
        if (activeModifier->handlePause() == true)
          return;
      if (ignorePauseButton == false)
        if (player::isPlaying()) {
          mp3.pause();
          setStandbyTimer();
        }
        else if (knownCard) {
          mp3.start();
          disableStandbyTimer();
        }
      ignorePauseButton = false;
    } else if (pauseButton.pressedFor(LONG_PRESS) &&
               ignorePauseButton == false) {
      if (activeModifier != NULL)
        if (activeModifier->handlePause() == true)
          return;
      if (player::isPlaying()) {
        uint8_t advertTrack;
        if (myFolder->playmode == PlayMode::_3_RandomsInFolder || myFolder->playmode == PlayMode::_9_FromTo_RandomTracks) {
          advertTrack = (player::queue[player::currentTrack - 1]);
        }
        else {
          advertTrack = player::currentTrack;
        }
        // Spezialmodus Von-Bis für Album und Party gibt die Dateinummer relativ zur Startposition wieder
        if (myFolder->playmode == PlayMode::_8_FromTo_AllTracks || myFolder->playmode == PlayMode::_9_FromTo_RandomTracks) {
          advertTrack = advertTrack - myFolder->from_track + 1;
        }
        mp3.playAdvertisement(getAdvertNumber(advertTrack));
      }
      else {
        playShortCut(0);
      }
      ignorePauseButton = true;
    }

    if (upButton.pressedFor(LONG_PRESS)) {
#ifndef USE_FIVEBUTTONS
      if (player::isPlaying()) {
        if (!mySettings.invertVolumeButtons) {
          volumeUpButton();
        }
        else {
          nextButton();
        }
      }
      else {
        playShortCut(1);
      }
      ignoreUpButton = true;
#endif
    } else if (upButton.wasReleased()) {
      if (!ignoreUpButton)
        if (!mySettings.invertVolumeButtons) {
          nextButton();
        }
        else {
          volumeUpButton();
        }
      ignoreUpButton = false;
    }

    if (downButton.pressedFor(LONG_PRESS)) {
#ifndef USE_FIVEBUTTONS
      if (player::isPlaying()) {
        if (!mySettings.invertVolumeButtons) {
          volumeDownButton();
        }
        else {
          previousButton();
        }
      }
      else {
        playShortCut(2);
      }
      ignoreDownButton = true;
#endif
    } else if (downButton.wasReleased()) {
      if (!ignoreDownButton) {
        if (!mySettings.invertVolumeButtons) {
          previousButton();
        }
        else {
          volumeDownButton();
        }
      }
      ignoreDownButton = false;
    }
#ifdef USE_FIVEBUTTONS
    if (buttonFour.wasReleased()) {
      if (player::isPlaying()) {
        if (!mySettings.invertVolumeButtons) {
          volumeUpButton();
        }
        else {
          nextButton();
        }
      }
      else {
        playShortCut(1);
      }
    }
    if (buttonFive.wasReleased()) {
      if (player::isPlaying()) {
        if (!mySettings.invertVolumeButtons) {
          volumeDownButton();
        }
        else {
          previousButton();
        }
      }
      else {
        playShortCut(2);
      }
    }
#endif
    // Ende der Buttons
  } while (!mfrc522.PICC_IsNewCardPresent());

  // RFID Karte wurde aufgelegt

  if (!mfrc522.PICC_ReadCardSerial())
    return;

  if (readCard(&myCard) == true) {
    if (myCard.cookie == TONUINO_COOKIE && 
        myCard.nfcFolderSettings.folder != 0 && 
        myCard.nfcFolderSettings.playmode != PlayMode::_0_None) {
      mp3.playAdvertisement(Advertisements::NOTIFIER_DING);
      playFolder();
    }

    // Neue Karte konfigurieren
    else if (myCard.cookie != TONUINO_COOKIE) {
      knownCard = false;
      mp3.playMp3FolderTrack(Mp3s::OH_A_NEW_CARD);
      player::waitForTrackToFinish();
      setupCard();
    }
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

void adminMenu(bool fromCard = false) {
  disableStandbyTimer();
  mp3.pause();
  Serial.println(F("=== adminMenu()"));
  knownCard = false;
  if (fromCard == false)
    // TODO: if (checkAdminAccess() == false)
      //return; 
  {
    // Admin menu has been locked - it still can be trigged via admin card
    if (mySettings.adminMenuLocked == LockMode::CardLock) {
      return;
    }
    // Pin check
    else if (mySettings.adminMenuLocked == LockMode::SequenceLock) {
      uint8_t sequence[4];
      mp3.playMp3FolderTrack(991); // Bitte gebe die PIN ein!
      if (askCode(sequence) == true) {
        if (compareBytes(sequence, mySettings.lockSequence) == false) {
          mp3.playMp3FolderTrack(401); // Oh weh! Das hat leider nicht geklappt!
          return;
        }
      } else {
        return;
      }
    }
    // Match check
    else if (mySettings.adminMenuLocked == LockMode::CalcLock) {
      uint8_t a = random(10, 20);
      uint8_t b = random(1, 10);
      uint8_t c;
      mp3.playMp3FolderTrack(992);
      player::waitForTrackToFinish();
      mp3.playMp3FolderTrack(getMp3Number(a));

      if (random(1, 3) == 2) {
        // a + b
        c = a + b;
        player::waitForTrackToFinish();
        mp3.playMp3FolderTrack(993);
      } else {
        // a - b
        b = random(1, a);
        c = a - b;
        player::waitForTrackToFinish();
        mp3.playMp3FolderTrack(994);
      }
      player::waitForTrackToFinish();
      mp3.playMp3FolderTrack(getMp3Number(b));
      Serial.println(c);
      uint8_t temp = voiceMenu(255, 0, 0, false);
      if (temp != c) {
        if (temp) mp3.playMp3FolderTrack(401); // Oh weh! Das hat leider nicht geklappt!
        return;
      }
    }
  }
  int subMenu = voiceMenu(12, 900, 900, false, false, 0, true);
  if (subMenu == 0)
    return;
  if (subMenu == 1) {
    resetCard();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
  else if (subMenu == 2) {
    // Maximum Volume
    mySettings.maxVolume = voiceMenu(30 - mySettings.minVolume, 930, mySettings.minVolume, false, false, mySettings.maxVolume - mySettings.minVolume) + mySettings.minVolume;
  }
  else if (subMenu == 3) {
    // Minimum Volume
    mySettings.minVolume = voiceMenu(mySettings.maxVolume - 1, 931, 0, false, false, mySettings.minVolume);
  }
  else if (subMenu == 4) {
    // Initial Volume
    mySettings.initVolume = voiceMenu(mySettings.maxVolume - mySettings.minVolume + 1, 932, mySettings.minVolume - 1, false, false, mySettings.initVolume - mySettings.minVolume + 1) + mySettings.minVolume - 1;
  }
  else if (subMenu == 5) {
    // EQ
    mySettings.eq = voiceMenu(6, 920, 920, false, false, mySettings.eq);
    mp3.setEq(mySettings.eq - 1);
  }
  else if (subMenu == 6) {
    // create modifier card
    NfcTag tempCard;
    tempCard.cookie = TONUINO_COOKIE;
    tempCard.version = 1;
    tempCard.nfcFolderSettings.folder = 0;
    tempCard.nfcFolderSettings.track = 0;
    tempCard.nfcFolderSettings.to_track = 0;
    tempCard.nfcFolderSettings.playmode = (PlayMode) voiceMenu(6, 970, 970, false, false, 0, true);

    if (tempCard.nfcFolderSettings.playmode != (PlayMode) PlayModifier::_0_None) {
      if (tempCard.nfcFolderSettings.playmode == (PlayMode) PlayModifier::_1_Sleeptimer) {
        switch (voiceMenu(4, 960, 960)) {
          case 1: tempCard.nfcFolderSettings.sleep_timeout = 5; break;
          case 2: tempCard.nfcFolderSettings.sleep_timeout = 15; break;
          case 3: tempCard.nfcFolderSettings.sleep_timeout = 30; break;
          case 4: tempCard.nfcFolderSettings.sleep_timeout = 60; break;
        }
      }
      mp3.playMp3FolderTrack(800);
      do {
        readButtons();
        if (upButton.wasReleased() || downButton.wasReleased()) {
          Serial.println(F("Abort!"));
          mp3.playMp3FolderTrack(802);
          return;
        }
      } while (!mfrc522.PICC_IsNewCardPresent());

      // RFID Karte wurde aufgelegt
      if (mfrc522.PICC_ReadCardSerial()) {
        Serial.println(F("Writing card..."));
        writeCard(tempCard);
        delay(100);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        player::waitForTrackToFinish();
      }
    }
  }
  else if (subMenu == 7) {
    uint8_t shortcut = voiceMenu(4, 940, 940);
    FolderSetting& favFolder = mySettings.shortCuts[shortcut - 1];
    if (setupFolder(favFolder)) {
       mp3.playMp3FolderTrack(Mp3s::OK);
    } else {
       mp3.playMp3FolderTrack(Mp3s::THAT_DIDNT_WORK);
       favFolder.folder = 0;
       favFolder.playmode = PlayMode::_0_None;
    }
    
  }
  else if (subMenu == 8) {
    switch (voiceMenu(5, 960, 960)) {
      case 1: mySettings.standbyTimer = 5; break;
      case 2: mySettings.standbyTimer = 15; break;
      case 3: mySettings.standbyTimer = 30; break;
      case 4: mySettings.standbyTimer = 60; break;
      case 5: mySettings.standbyTimer = 0; break;
    }
  }
  else if (subMenu == 9) {
    // Create Cards for Folder
    // Ordner abfragen
    NfcTag tempCard;
    tempCard.cookie = TONUINO_COOKIE;
    tempCard.version = 1;
    tempCard.nfcFolderSettings.playmode = PlayMode::_4_TrackInFolder;
    tempCard.nfcFolderSettings.folder = voiceMenu(99, 301, 0, true);
    uint8_t from_track = voiceMenu(mp3.getFolderTrackCount(tempCard.nfcFolderSettings.folder), 321, 0,
                                true, tempCard.nfcFolderSettings.folder);
    uint8_t to_track = voiceMenu(mp3.getFolderTrackCount(tempCard.nfcFolderSettings.folder), 322, 0,
                                 true, tempCard.nfcFolderSettings.folder, from_track);

    mp3.playMp3FolderTrack(936);
    player::waitForTrackToFinish();
    for (uint8_t x = from_track; x <= to_track; x++) {
      mp3.playMp3FolderTrack(getMp3Number(x));
      tempCard.nfcFolderSettings.from_track = x;
      Serial.print(x);
      Serial.println(F("Karte auflegen"));
      do {
        readButtons();
        if (upButton.wasReleased() || downButton.wasReleased()) {
          Serial.println(F("Abgebrochen!"));
          mp3.playMp3FolderTrack(802);
          return;
        }
      } while (!mfrc522.PICC_IsNewCardPresent());

      // RFID Karte wurde aufgelegt
      if (mfrc522.PICC_ReadCardSerial()) {
        Serial.println(F("schreibe Karte..."));
        writeCard(tempCard);
        delay(100);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        player::waitForTrackToFinish();
      }
    }
  }
  else if (subMenu == 10) {
    // Invert Functions for Up/Down Buttons
    int temp = voiceMenu(2, 933, 933, false);
    if (temp == 2) {
      mySettings.invertVolumeButtons = true;
    }
    else {
      mySettings.invertVolumeButtons = false;
    }
  }
  else if (subMenu == 11) {
    // Reset to factory defaults
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
    config::resetToFactoryDefaults(mySettings);
    mp3.playMp3FolderTrack(999);
  }
  else if (subMenu == 12) {
    // lock admin menu
    int temp = voiceMenu(4, 980, 980, false);
    if (temp == 1) {
      mySettings.adminMenuLocked = LockMode::NoLock;
    }
    else if (temp == 2) {
      mySettings.adminMenuLocked = LockMode::CardLock;
    }
    else if (temp == 3) {
      uint8_t sequence[4];
      mp3.playMp3FolderTrack(991);
      if (askCode(sequence)) {
        memcpy(mySettings.lockSequence, sequence, 4);
        mySettings.adminMenuLocked = LockMode::SequenceLock;
      }
    }
    else if (temp == 4) {
      mySettings.adminMenuLocked = LockMode::CalcLock;
    }

  }
  config::writeToFlash(mySettings);
  setStandbyTimer();
}

bool askCode(uint8_t * code, uint8_t codeLength = 4) {
  uint8_t i = 0;
  while (i < codeLength) {
    readButtons();
    if (pauseButton.pressedFor(LONG_PRESS))
      break;
    if (pauseButton.wasReleased())
      code[i++] = 1;
    if (upButton.wasReleased())
      code[i++] = 2;
    if (downButton.wasReleased())
      code[i++] = 3;
    #ifdef USE_FIVEBUTTONS
    if (buttonFour.wasReleased()){
      code[i++] = 4;
    }
    if (buttonFive.wasReleased()){
      code[i++] = 5;
    }
    #endif
  }
  return true;
}

/**
 * @returns defaultValue in case user aborts menu
 */
uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview = false, int previewFromFolder = 0, int defaultValue = 0, bool exitWithLongPress = false) {
  uint8_t returnValue = defaultValue;
  if (startMessage != 0)
    mp3.playMp3FolderTrack(startMessage);
  Serial.print(F("=== voiceMenu() ("));
  Serial.print(numberOfOptions);
  Serial.println(F(" Options)"));
  do {
    if (Serial.available() > 0) {
      int optionSerial = Serial.parseInt();
      if (optionSerial != 0 && optionSerial <= numberOfOptions)
        return optionSerial;
    }
    readButtons();
    mp3.loop();
    if (pauseButton.pressedFor(LONG_PRESS)) {
      mp3.playMp3FolderTrack(802); // Okay, ich habe den Vorgang abgebrochen!
      ignorePauseButton = true;
      checkStandbyAndShutdown();
      return defaultValue;
    }
    if (pauseButton.wasReleased()) {
      if (returnValue != 0) {
        Serial.print(F("=== "));
        Serial.print(returnValue);
        Serial.println(F(" ==="));
        return returnValue;
      }
      delay(1000);
    }

    if (upButton.pressedFor(LONG_PRESS)) {
      returnValue = min(returnValue + 10, numberOfOptions);
      Serial.println(returnValue);
      //mp3.pause();
      mp3.playMp3FolderTrack(messageOffset + returnValue);
      player::waitForTrackToFinish();
      /*if (preview) {
        if (previewFromFolder == 0)
          mp3.playFolderTrack(returnValue, 1);
        else
          mp3.playFolderTrack(previewFromFolder, returnValue);
        }*/
      ignoreUpButton = true;
    } else if (upButton.wasReleased()) {
      if (!ignoreUpButton) {
        returnValue = min(returnValue + 1, numberOfOptions);
        Serial.println(returnValue);
        //mp3.pause();
        mp3.playMp3FolderTrack(messageOffset + returnValue);
        if (preview) {
          player::waitForTrackToFinish();
          if (previewFromFolder == 0) {
            mp3.playFolderTrack(returnValue, 1);
          } else {
            mp3.playFolderTrack(previewFromFolder, returnValue);
          }
          delay(1000);
        }
      } else {
        ignoreUpButton = false;
      }
    }

    if (downButton.pressedFor(LONG_PRESS)) {
      returnValue = max(returnValue - 10, 1);
      Serial.println(returnValue);
      //mp3.pause();
      mp3.playMp3FolderTrack(messageOffset + returnValue);
      player::waitForTrackToFinish();
      /*if (preview) {
        if (previewFromFolder == 0)
          mp3.playFolderTrack(returnValue, 1);
        else
          mp3.playFolderTrack(previewFromFolder, returnValue);
        }*/
      ignoreDownButton = true;
    } else if (downButton.wasReleased()) {
      if (!ignoreDownButton) {
        returnValue = max(returnValue - 1, 1);
        Serial.println(returnValue);
        //mp3.pause();
        mp3.playMp3FolderTrack(messageOffset + returnValue);
        if (preview) {
          player::waitForTrackToFinish();
          if (previewFromFolder == 0) {
            mp3.playFolderTrack(returnValue, 1);
          }
          else {
            mp3.playFolderTrack(previewFromFolder, returnValue);
          }
          delay(1000);
        }
      } else {
        ignoreDownButton = false;
      }
    }
  } while (true);
}

void resetCard() {
  mp3.playMp3FolderTrack(800); // Bitte lege nun die Karte auf!
  do {
    readButtons();

    if (upButton.wasReleased() || downButton.wasReleased()) {
      Serial.print(F("Abort!"));
      mp3.playMp3FolderTrack(802); // Ok, ich habe den Vorgang abgebrochen!
      return;
    }
  } while (!mfrc522.PICC_IsNewCardPresent());

  if (!mfrc522.PICC_ReadCardSerial())
    return;

  Serial.print(F("Karte wird neu konfiguriert!"));
  setupCard();
}

bool setupFolder(FolderSetting& theFolder) {
  // Ordner abfragen
  theFolder.folder = voiceMenu(player::numFolders, 301, 0, true, 0, 0, true);
  if (theFolder.folder == 0) return false;

  // Wiedergabemodus abfragen
  theFolder.playmode = (PlayMode) voiceMenu((int)PlayMode::LAST, 310, 310, false, 0, 0, true);
  if ((uint8_t) theFolder.playmode == 0) return false;

  //  // Hörbuchmodus -> Fortschritt im EEPROM auf 1 setzen
  //  EEPROM.update(theFolder->folder, 1);

  // Einzelmodus -> Datei abfragen
  if (theFolder.playmode == PlayMode::_4_TrackInFolder)
    theFolder.track = voiceMenu(mp3.getFolderTrackCount(theFolder.folder), 320, 0,
                                   true, theFolder.folder);
  // Admin Funktionen
  if (theFolder.playmode == PlayMode::_6_AdminCard) {
    //theFolder->track = voiceMenu(3, 320, 320);
    theFolder.folder = 0;
    theFolder.playmode = (PlayMode) 255;
  }
  // Spezialmodus Von-Bis
  if (theFolder.playmode == PlayMode::_7_FromTo_RandomTrack || 
      theFolder.playmode == PlayMode::_8_FromTo_AllTracks || 
      theFolder.playmode == PlayMode::_9_FromTo_RandomTracks) {
    theFolder.from_track = voiceMenu(mp3.getFolderTrackCount(theFolder.folder), 321, 0,
                                   true, theFolder.folder);
    theFolder.to_track = voiceMenu(mp3.getFolderTrackCount(theFolder.folder), 322, 0,
                                    true, theFolder.folder, theFolder.from_track);
  }
  return true;
}


void setupCard() {
  mp3.pause();
  Serial.println(F("=== setupCard()"));
  NfcTag newCard;
  if (setupFolder(newCard.nfcFolderSettings) == true)
  {
    // Karte ist konfiguriert -> speichern
    mp3.pause();
    do {
    } while (player::isPlaying());
    writeCard(newCard);
  }
  delay(1000);
}

/**
 * @returns True on success
 */ 
bool readCard(NfcTag * nfcTag) {
  NfcTag tempCard;
  // Show some details of the PICC (that is: the tag/card)
  Serial.print(F("Card UID:"));
  dump_byte_array(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.println();
  Serial.print(F("PICC type: "));
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.println(mfrc522.PICC_GetTypeName(piccType));

  byte buffer[18];
  byte size = sizeof(buffer);

  // Authenticate using key A
  if ((piccType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.println(F("Authenticating Classic using key A..."));
    rfid::status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_A, rfid::trailerBlock, &rfid::key, &(mfrc522.uid));
  }
  else if (piccType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte pACK[] = {0, 0}; //16 bit PassWord ACK returned by the tempCard

    // Authenticate using key A
    Serial.println(F("Authenticating MIFARE UL..."));
    rfid::status = mfrc522.PCD_NTAG216_AUTH(rfid::key.keyByte, pACK);
  }

  if (rfid::status != MFRC522::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(rfid::status));
    return false;
  }

  // Read data from the block
  if ((piccType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.print(F("Reading data from block "));
    Serial.print(rfid::blockAddr);
    Serial.println(F(" ..."));
    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(rfid::blockAddr, buffer, &size);
    if (rfid::status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(rfid::status));
      return false;
    }
  }
  else if (piccType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte buffer2[18];
    byte size2 = sizeof(buffer2);

    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(8, buffer2, &size2);
    if (rfid::status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_1() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(rfid::status));
      return false;
    }
    memcpy(buffer, buffer2, 4);

    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(9, buffer2, &size2);
    if (rfid::status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_2() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(rfid::status));
      return false;
    }
    memcpy(buffer + 4, buffer2, 4);

    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(10, buffer2, &size2);
    if (rfid::status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_3() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(rfid::status));
      return false;
    }
    memcpy(buffer + 8, buffer2, 4);

    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(11, buffer2, &size2);
    if (rfid::status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_4() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(rfid::status));
      return false;
    }
    memcpy(buffer + 12, buffer2, 4);
  }

  Serial.print(F("Data on Card "));
  Serial.println(F(":"));
  dump_byte_array(buffer, 16);
  Serial.println();
  Serial.println();

  uint32_t tempCookie;
  tempCookie = (uint32_t)buffer[0] << 24;
  tempCookie += (uint32_t)buffer[1] << 16;
  tempCookie += (uint32_t)buffer[2] << 8;
  tempCookie += (uint32_t)buffer[3];

  tempCard.cookie = tempCookie;
  tempCard.version = buffer[4];
  tempCard.nfcFolderSettings.folder = buffer[5];
  tempCard.nfcFolderSettings.playmode = (PlayMode) buffer[6];
  tempCard.nfcFolderSettings.from_track = buffer[7];
  tempCard.nfcFolderSettings.to_track = buffer[8];

  if (tempCard.cookie == TONUINO_COOKIE) {

    if (activeModifier != NULL && tempCard.nfcFolderSettings.folder != 0) {
      if (activeModifier->handleRFID(&tempCard) == true) {
        return false;
      }
    }

    if (tempCard.nfcFolderSettings.folder == 0) {
      if (activeModifier != NULL) {
        if (activeModifier->getActive() == (uint8_t) tempCard.nfcFolderSettings.playmode) {
          activeModifier = NULL;
          Serial.println(F("modifier removed"));
          if (player::isPlaying()) {
            mp3.playAdvertisement(Advertisements::NOTIFIER_DONG);
          }
          else {
            mp3.start();
            delay(100);
            mp3.playAdvertisement(Advertisements::NOTIFIER_DONG);
            delay(100);
            mp3.pause();
          }
          delay(2000);
          return false;
        }
      }
      if (tempCard.nfcFolderSettings.playmode != (PlayMode)0 && 
          tempCard.nfcFolderSettings.playmode != (PlayMode)255) {
        if (player::isPlaying()) {
          mp3.playAdvertisement(Advertisements::NOTIFIER_DING);
        }
        else {
          mp3.start();
          delay(100);
          mp3.playAdvertisement(Advertisements::NOTIFIER_DING);
          delay(100);
          mp3.pause();
        }
      }
      switch ((uint8_t) tempCard.nfcFolderSettings.playmode ) {
        case 0:
        case 255:
          mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1(); adminMenu(true);  break;
        case 1: activeModifier = new SleepTimer(tempCard.nfcFolderSettings.sleep_timeout); break;
        case 2: activeModifier = new FreezeDance(); break;
        case 3: activeModifier = new Locked(); break;
        case 4: activeModifier = new ToddlerMode(); break;
        case 5: activeModifier = new KindergardenMode(); break;
        case 6: activeModifier = new RepeatSingleModifier(); break;

      }
      delay(2000);
      return false;
    }
    else {
      memcpy(nfcTag, &tempCard, sizeof(NfcTag));
      Serial.println( nfcTag->nfcFolderSettings.folder);
      myFolder = &nfcTag->nfcFolderSettings;
      Serial.println( myFolder->folder);
    }
    return true;
  }
  else {
    memcpy(nfcTag, &tempCard, sizeof(NfcTag));
    return true;
  }
}


void writeCard(NfcTag nfcTag) {
  MFRC522::PICC_Type mifareType;
  byte buffer[16] = {0x13, 0x37, 0xb3, 0x47, // 0x1337 0xb347 magic cookie to
                     // identify our nfc tags
                     0x02,                   // version 1
                     nfcTag.nfcFolderSettings.folder,          // the folder picked by the user
                     (byte) nfcTag.nfcFolderSettings.playmode,    // the playback mode picked by the user
                     nfcTag.nfcFolderSettings.track, // track or function for admin cards
                     nfcTag.nfcFolderSettings.to_track,
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                    };

  byte size = sizeof(buffer);

  mifareType = mfrc522.PICC_GetType(mfrc522.uid.sak);

  // Authenticate using key B
  //authentificate with the card and set card specific parameters
  if ((mifareType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.println(F("Authenticating again using key A..."));
    rfid::status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_A, rfid::trailerBlock, &rfid::key, &(mfrc522.uid));
  }
  else if (mifareType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte pACK[] = {0, 0}; //16 bit PassWord ACK returned by the NFCtag

    // Authenticate using key A
    Serial.println(F("Authenticating UL..."));
    rfid::status = mfrc522.PCD_NTAG216_AUTH(rfid::key.keyByte, pACK);
  }

  if (rfid::status != MFRC522::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(rfid::status));
    mp3.playMp3FolderTrack(401);
    return;
  }

  // Write data to the block
  Serial.print(F("Writing data into block "));
  Serial.print(rfid::blockAddr);
  Serial.println(F(" ..."));
  dump_byte_array(buffer, 16);
  Serial.println();

  if ((mifareType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(rfid::blockAddr, buffer, 16);
  }
  else if (mifareType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte buffer2[16];
    byte size2 = sizeof(buffer2);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer, 4);
    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(8, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 4, 4);
    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(9, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 8, 4);
    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(10, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 12, 4);
    rfid::status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(11, buffer2, 16);
  }

  if (rfid::status != MFRC522::STATUS_OK) {
    Serial.print(F("MIFARE_Write() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(rfid::status));
    mp3.playMp3FolderTrack(Mp3s::THAT_DIDNT_WORK);
  }
  else
    mp3.playMp3FolderTrack(Mp3s::OK);
  Serial.println();
  delay(2000);
}


/**
 * @returns 32-bit random number from LSB of unconnected analog input 
 */
long calcRandomSeed(){
  // Wert für randomSeed() erzeugen durch das mehrfache Sammeln von rauschenden LSBs eines offenen Analogeingangs
  uint32_t ADC_LSB;
  uint32_t ADCSeed;
  for (uint8_t i = 0; i < 128; i++) {
    ADC_LSB = analogRead(OPENANALOG_PIN) & 0x1;
    ADCSeed ^= ADC_LSB << (i % 32);
  }
  return ADCSeed; // Zufallsgenerator initialisieren
}


/**
  Helper routine to dump a byte array as hex values to Serial.
*/
void dump_byte_array(byte * buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
}


/**
 * @returns True if all bytes in array a and b are equal
 */ 
bool compareBytes ( uint8_t a[], uint8_t b[], byte bufferSize = 4 ) {
  for (byte i = 0; i < bufferSize; i++) {
    if ( a[i] != b[i] ) { 
      return false;
    }
  }
  return true;
}
