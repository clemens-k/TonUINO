// Arduino Button Library
// https://github.com/JChristensen/JC_Button
// Copyright (C) 2018 by Jack Christensen and licensed under
// GNU GPL v3.0, https://www.gnu.org/licenses/gpl.html
//
// Example sketch to turn an LED on and off with a tactile button switch.
// Wire the switch from the Arduino pin to ground.

#include <JC_Button.h>          // https://github.com/JChristensen/JC_Button
#include <SoftwareSerial.h>
    
// pin assignments
const byte
    BUTTON_PIN(A0),              // connect a button switch from this pin to ground
    LED_PIN(13);                // the standard Arduino "pin 13" LED

Button Button_A0(A0);           // define the button
Button Button_A1(A1);           // define the button
Button Button_A2(A2);           // define the button

void setup()
{
    Serial.begin(115200); // Es gibt ein paar Debug Ausgaben über die serielle Schnittstelle

    Serial.println(F("\nTonUINO Button Test Program\n"));
  
    Button_A0.begin();              // initialize the button object
    Button_A1.begin();              // initialize the button object
    Button_A2.begin();              // initialize the button object
    pinMode(LED_PIN, OUTPUT);   // set the LED pin as an output
}

void loop()
{
    Button_A0.read();               // read the button
    Button_A1.read();
    Button_A2.read();

    if (Button_A0.wasReleased()) {
        Serial.println(F("A0 released!\n"));
    }
    if (Button_A0.wasPressed()) {
        Serial.println(F("A0 pressed!\n"));
    }

    if (Button_A1.wasReleased()) {
        Serial.println(F("A1 released!\n"));
    }
    if (Button_A1.wasPressed()) {
        Serial.println(F("A1 pressed!\n"));
    }

    if (Button_A2.wasReleased()) {
        Serial.println(F("A2 released!\n"));
    }
    if (Button_A2.wasPressed()) {
        Serial.println(F("A2 pressed!\n"));
    }
delay(100);
}
