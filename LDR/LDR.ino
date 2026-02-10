int ldrPin = A0;        // LDR connected to analog pin A0
int ledPin = 9;         // LED connected to digital pin 9
int ldrValue = 0;       // Variable to store LDR value
int threshold = 500;    // Adjust according to light level

void setup() {
  pinMode(ledPin, OUTPUT);
  Serial.begin(9600);   // Start Serial Monitor
}

void loop() {
  ldrValue = analogRead(ldrPin);   // Read LDR value
  Serial.println(ldrValue);        // Display value for calibration

  if (ldrValue < threshold) {
    digitalWrite(ledPin, LOW);    // LED ON (dark)
  } else {
    digitalWrite(ledPin, HIGH);     // LED OFF (bright)
  }

  delay(1);                      // Small delay
}
