#include <Servo.h>

Servo myServo;

void setup() {
    Serial.begin(9600);

    pinMode(4, INPUT_PULLUP);
    pinMode(5, OUTPUT); // Red LED
    pinMode(6, OUTPUT); // Blue LED
    pinMode(7, OUTPUT); // Green LED

    myServo.attach(9);
    myServo.write(90); 
}

void loop() {
    // Only send sensor data once every 200ms to clear up the Serial line
    static unsigned long lastTime = 0;
    if (millis() - lastTime > 200) {
        lastTime = millis();
        
        int potValue = analogRead(A0);
        int photoValue = analogRead(A2);
        int buttValue = !(digitalRead(4));

        // Prefix your telemetry so your computer can distinguish it from echoes
        Serial.print("DATA:");
        Serial.print(potValue);
        Serial.print(",");
        Serial.print(buttValue);
        Serial.print(",");
        Serial.println(photoValue);
    }

    handleSerialCommands();
}

void handleSerialCommands() {
    if (Serial.available() > 0) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim(); 

        if (cmd.length() == 0) return;

        char type = cmd.charAt(0);

        if (type == 'G') {
            int ledState = cmd.substring(1).toInt();
            digitalWrite(7, ledState ? HIGH : LOW);
            Serial.print("LED Green set to ");
            Serial.println(ledState);
        }
        else if (type == 'B') {
            int ledState = cmd.substring(1).toInt();
            digitalWrite(6, ledState ? HIGH : LOW);
            Serial.print("LED Blue set to ");
            Serial.println(ledState);
        }
        else if (type == 'R') {
            int ledState = cmd.substring(1).toInt();
            digitalWrite(5, ledState ? HIGH : LOW);
            Serial.print("LED Red set to ");
            Serial.println(ledState);
        }
        else if (type == 'S') {
            int angle = cmd.substring(1).toInt();
            angle = constrain(angle, 0, 180); // Servo max is 180
            myServo.write(angle);
            Serial.print("Servo set to ");
            Serial.println(angle);
        }
        else {
            Serial.print("Unknown command: ");
            Serial.println(cmd);
        }
    }
}
