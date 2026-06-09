#include <Arduino.h>
#include <stdio.h>

// Broches pour le premier driver (moteur 1)
const int PIN_EN = 8;   // GP8  -> EN (enable) du TMC2209 (actif LOW)
const int PIN_STEP = 9; // GP9  -> STEP du TMC2209 (impulsion pour avancer d'un pas)
const int PIN_DIR = 10; // GP10 -> DIR du TMC2209 (sens de rotation)

// Broches pour le second driver (moteur 2)
const int PIN_EN2 = 18;   // GP18 -> EN TMC2209 (actif LOW)
const int PIN_STEP2 = 19; // GP19 -> STEP TMC2209
const int PIN_DIR2 = 20;  // GP20 -> DIR TMC2209

// TIR
const int PIN_TIR = 15;

int motor1Dir = HIGH;
int motor1Delay = 0;
int motor2Dir = HIGH;
int motor2Delay = 0;
bool fireCommand = false;
unsigned long motor1NextStepUs = 0;
unsigned long motor2NextStepUs = 0;

// -------------------- Fonction de déplacement --------------------
// moveNEMA : génère les impulsions STEP pour deux moteurs en parallèle.
// Paramètres :
//  - steps  : nombre d'impulsions (pas) à effectuer pour le moteur 1
//  - delayUs: délai en microsecondes entre les changements d'état de STEP (vitesse moteur 1)
//  - dir    : sens de rotation pour le moteur 1 (HIGH/LOW)
//  - steps2 : nombre d'impulsions pour le moteur 2
//  - delayUs2: délai en microsecondes entre les changements d'état de STEP (vitesse moteur 2)
//  - dir2   : sens de rotation pour le moteur 2 (HIGH/LOW)
// Remarque : dans la fonction actuelle, on utilise `delayUs` pour les deux moteurs
// au moment d'appeler depuis `loop()` (voir commentaire sur le parsing série).
void moveNEMA(int steps, int delayUs, bool dir, int steps2, int delayUs2, bool dir2)
{
    digitalWrite(PIN_DIR, dir);
    digitalWrite(PIN_DIR2, dir2);

    for (int i = 0; i < steps || i < steps2; i++)
    {
        if (i < steps)
        {
            digitalWrite(PIN_STEP, HIGH);
        }
        if (i < steps2)
        {
            digitalWrite(PIN_STEP2, HIGH);
        }

        delayMicroseconds(delayUs);

        if (i < steps)
        {
            digitalWrite(PIN_STEP, LOW);
        }
        if (i < steps2)
        {
            digitalWrite(PIN_STEP2, LOW);
        }

        delayMicroseconds(delayUs);
    }
}

void updateMotor(int stepPin, int dirPin, int dir, int delayUs, unsigned long &nextStepUs)
{
    if (delayUs <= 0)
    {
        digitalWrite(stepPin, LOW);
        return;
    }

    digitalWrite(dirPin, dir ? HIGH : LOW);

    unsigned long now = micros();
    if ((long)(now - nextStepUs) >= 0)
    {
        digitalWrite(stepPin, HIGH);
        delayMicroseconds(2);
        digitalWrite(stepPin, LOW);
        nextStepUs = now + (unsigned long)delayUs;
    }
}

void processFireCommand(bool fire)
{
    if (!fire)
    {
        return;
    }
    digitalWrite(PIN_TIR, HIGH);
    delay(65);
    digitalWrite(PIN_TIR, LOW);
    Serial.write("Fire");
}

void parseCommand(const String &input)
{
    int dir1 = 0;
    int delay1 = 0;
    int dir2 = 0;
    int delay2 = 0;
    int fire_command = 0;

    if (sscanf(input.c_str(), "%d,%d,%d,%d,%d", &dir1, &delay1, &dir2, &delay2, &fire_command) == 5)
    {
        motor1Dir = dir1;
        motor1Delay = delay1;
        motor2Dir = dir2;
        motor2Delay = delay2;
        fireCommand = fire_command != false;

        if (motor1Delay <= 0)
        {
            digitalWrite(PIN_STEP, LOW);
        }
        if (motor2Delay <= 0)
        {
            digitalWrite(PIN_STEP2, LOW);
        }
    }
}

void setup()
{
    pinMode(PIN_EN, OUTPUT);
    pinMode(PIN_STEP, OUTPUT);
    pinMode(PIN_DIR, OUTPUT);
    pinMode(PIN_EN2, OUTPUT);
    pinMode(PIN_STEP2, OUTPUT);
    pinMode(PIN_DIR2, OUTPUT);
    pinMode(PIN_TIR, OUTPUT);

    digitalWrite(PIN_STEP, LOW);
    digitalWrite(PIN_STEP2, LOW);

    digitalWrite(PIN_DIR, HIGH);
    digitalWrite(PIN_DIR2, HIGH);

    digitalWrite(PIN_EN, LOW);
    digitalWrite(PIN_EN2, LOW);

    Serial.begin(115200);
    Serial.println("Démarrage...");

    delay(500);
}

void loop()
{
    if (Serial.available() > 0)
    {
        String input = Serial.readStringUntil('\n');
        parseCommand(input);
    }

    processFireCommand(fireCommand);
    updateMotor(PIN_STEP, PIN_DIR, motor1Dir, motor1Delay * 4, motor1NextStepUs);
    updateMotor(PIN_STEP2, PIN_DIR2, motor2Dir, motor2Delay * 4, motor2NextStepUs);
}
