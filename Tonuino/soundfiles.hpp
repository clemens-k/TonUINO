#pragma once

// in "advert" folder numbers 1..255 are stored in file 0001.mp3..0255.mp3
uint16_t getAdvertNumber(uint8_t numberToSay){
    return numberToSay;
}

// in "mp3" folder numbers 1..255 are stored in file 0001.mp3..0255.mp3
uint16_t getMp3Number(uint8_t numberToSay){
    return numberToSay;
}

// all soundfiles in "advert" folder
enum Advertisements {
    NOTIFIER_DING = 260,
    NOTIFIER_DONG = 261,
    FREEZE_INTRO = 300,
    FREEZE_STOPP,
    FREEZE_DONT_MOVE,
    FREEZE_CONITNUE,
};

// all soundfiles in "mp3" folder
enum Mp3s {
    OH_A_NEW_CARD = 300,
    OK = 400,
    THAT_DIDNT_WORK = 401
};
