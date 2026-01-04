#include <TheThingsNetwork.h>

// Pin definitions
int sensorPin = A0;    // Input pin for the potentiometer
int sensorValue = 0;   // Variable to store sensor reading

// TTN credentials
const char *appEui = "0004A30B001E5DD8";
const char *appKey = "DE34455E0B747CA641D9D34CFA19040C";

// LoRa frequency plan
#define freqPlan TTN_FP_EU868

// Serial ports
#define loraSerial Serial1
#define debugSerial Serial

// ECG monitoring variables
long instance1 = 0, timer;
double hrv = 0, hr = 72, interval = 0;
double lastInterval = 0;               // Previous RR interval for HRV calculation
int count = 0;
bool flag = 0;
unsigned long lastDetectionTime = 0;    // Timestamp of last R-peak detection
unsigned long adaptiveRefractory = 250; // Adaptive refractory period (ms) - initialized to reasonable default

// Constants
#define threshold 240                   // R-peak threshold (scaled 0-1023 range)
#define timer_value 5000                // 5 seconds timer for HR calculation
#define MIN_REFRACTORY 150              // Minimum refractory period (400 BPM max)
#define MAX_REFRACTORY 600              // Maximum refractory period (100 BPM min)
#define HARDWARE_MAX_READING 588        // Actual max ADC reading from hardware
#define SCALE_TO_FULL_RANGE true        // Scale readings to 0-1023 range

// Buffer sizes
#define SIGNAL_BUFFER_SIZE 10
int signalBuffer[SIGNAL_BUFFER_SIZE];
int bufferIndex = 0;

// Heart rate history for averaging
#define HR_HISTORY_SIZE 10
double hrHistory[HR_HISTORY_SIZE];
int hrHistoryIndex = 0;
int hrHistoryCount = 0;
double averageHR = 0;
#define MIN_SENSOR_VALUE 0    // Minimum sensor reading
#define MAX_SENSOR_VALUE 1023 // Maximum sensor reading

// Timing constants
#define dataRate 1              // Loop delay in ms
#define transmissionRate 10000  // Send data every 10 seconds

unsigned long lastTransmission = 0;

// TTN object
TheThingsNetwork ttn(loraSerial, debugSerial, freqPlan);

void setup()
{
  // Initialize serial communication
  loraSerial.begin(57600);
  debugSerial.begin(9600);

  // Wait for Serial Monitor (max 10 seconds)
  while (!debugSerial && millis() < 10000)
    ;

  // Display TTN status
  debugSerial.println("-- STATUS");
  ttn.showStatus();

  // Join TTN network
  debugSerial.println("-- JOIN");
  ttn.join(appEui, appKey);

  // Initialize ECG timing variables
  timer = millis();
  instance1 = micros();

  // Initialize signal buffer
  for (int i = 0; i < SIGNAL_BUFFER_SIZE; i++)
  {
    signalBuffer[i] = 0;
  }

  // Initialize HR history
  for (int i = 0; i < HR_HISTORY_SIZE; i++)
  {
    hrHistory[i] = 0;
  }
}

// ============================================================
// SIGNAL PROCESSING FUNCTIONS
// ============================================================

/**
 * Read ECG sensor data
 * Scales hardware output to full 0-1023 range
 */
void readAndFilterSensor()
{
  int rawValue = analogRead(sensorPin);

#if SCALE_TO_FULL_RANGE
  // Scale to compensate for hardware voltage limitation
  sensorValue = ((long)rawValue * 1023) / HARDWARE_MAX_READING;
  if (sensorValue > 1023)
    sensorValue = 1023;
#else
  sensorValue = rawValue;
#endif
}

// ============================================================
// ECG ANALYSIS FUNCTIONS
// ============================================================

/**
 * Detect R-peaks in ECG signal and calculate HRV
 * Uses fixed threshold + adaptive refractory period
 */
void detectRPeakAndCalculateHRV()
{
  unsigned long currentTime = millis();

  // R-peak detection with fixed threshold
  if (sensorValue > threshold)
  {
    // Check adaptive refractory period
    if (currentTime - lastDetectionTime >= adaptiveRefractory)
    {
      count++;
      lastDetectionTime = currentTime;
      interval = micros() - instance1; // RR interval in microseconds

      // Update adaptive refractory period
      if (interval > 0)
      {
        unsigned long rrMillis = interval / 1000; // Convert to milliseconds
        adaptiveRefractory = rrMillis * 0.3;

        // Constrain to safe limits
        if (adaptiveRefractory < MIN_REFRACTORY)
          adaptiveRefractory = MIN_REFRACTORY;
        if (adaptiveRefractory > MAX_REFRACTORY)
          adaptiveRefractory = MAX_REFRACTORY;
      }

      // Calculate HRV
      if (lastInterval > 0 && interval > 0)
      {
        hrv = (interval - lastInterval) / 1000.0; // HRV in milliseconds
      }
      lastInterval = interval;
      instance1 = micros();
    }
  }
}

/**
 * Calculate heart rate from R-peak count
 */
void calculateHeartRate()
{
  if ((millis() - timer) > timer_value)
  {
    if (count > 0) {
      hr = count * 12; // Convert to BPM (60/5 = 12)
    } else {
      debugSerial.println("⚠️ No heartbeats detected in 5s");
      hr = 0;
    }
    
    timer = millis();
    count = 0;

    // Only update history with valid readings
    if (hr > 0) {
      // Update HR history
      hrHistory[hrHistoryIndex] = hr;
      hrHistoryIndex = (hrHistoryIndex + 1) % HR_HISTORY_SIZE;
      if (hrHistoryCount < HR_HISTORY_SIZE)
      {
        hrHistoryCount++;
      }

      // Calculate average heart rate
      double hrSum = 0;
      for (int i = 0; i < hrHistoryCount; i++)
      {
        hrSum += hrHistory[i];
      }
      averageHR = hrSum / hrHistoryCount;

      // Debug output
      debugSerial.print("Average HR (last ");
      debugSerial.print(hrHistoryCount);
      debugSerial.print(" readings): ");
      debugSerial.println(averageHR);
    }
  }
}

// ============================================================
// DATA TRANSMISSION FUNCTIONS
// ============================================================

/**
 * Create 6-byte payload with ECG data
 */
void buildPayload(byte *payload)
{
  // Raw sensor value (2 bytes)
  payload[0] = highByte(sensorValue);
  payload[1] = lowByte(sensorValue);

  // Heart rate scaled by 10 (2 bytes)
  int hrInt = (int)(hr * 10);
  payload[2] = highByte(hrInt);
  payload[3] = lowByte(hrInt);

  // HRV scaled by 1000 (2 bytes)
  int hrvInt = (int)(hrv * 1000);
  payload[4] = highByte(hrvInt);
  payload[5] = lowByte(hrvInt);
}

/**
 * Send data via LoRaWAN
 */
void transmitDataIfNeeded()
{
  if (millis() - lastTransmission > transmissionRate)
  {
    byte payload[6];
    buildPayload(payload);

    // Pause HR calculation timer
    unsigned long pauseTime = millis();

    // Send data
    ttn.sendBytes(payload, sizeof(payload));

    // Resume timer with compensation
    unsigned long transmissionDelay = millis() - pauseTime;
    timer += transmissionDelay;

    debugSerial.print("Transmission took: ");
    debugSerial.print(transmissionDelay);
    debugSerial.println("ms");

    lastTransmission = millis();
  }
}

// ============================================================
// DEBUG OUTPUT FUNCTION
// ============================================================

/**
 * Print current ECG metrics
 */
void printDebugInfo()
{
  debugSerial. print(sensorValue);
  debugSerial.print(" Threshold: ");
  debugSerial.print(threshold);
  debugSerial.print(" HR: ");
  debugSerial.print(hr);
  debugSerial.print(" Avg HR: ");
  debugSerial.print(averageHR);
  debugSerial.print(" HRV: ");
  debugSerial.println(hrv);
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  // 1. Read sensor data
  readAndFilterSensor();

  // 2. ECG analysis
  detectRPeakAndCalculateHRV();
  calculateHeartRate();

  // 3. Display debug info
  printDebugInfo();

  // 4. Transmit data
  transmitDataIfNeeded();

  // Loop delay
  delay(dataRate);
}
