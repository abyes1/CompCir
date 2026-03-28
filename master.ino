#include "config.h"
#include "certificates.h"
#include <PromLokiTransport.h>
#include <PrometheusArduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

// ==========================================
// 1. PIN DEFINITIONS & LOCAL GLOBALS
// ==========================================
const int MIC_PIN = A0;      
const int CURRENT_PIN = A1;  
const int CO_PIN = A2;       

Adafruit_BME680 bme; 
const float mV_PER_STEP = 3300.0 / 4095.0; 

// --- TIMERS ---
unsigned long lastCurrentSampleTime = 0;
unsigned long lastMicSampleTime = 0;
unsigned long lastMicCalcTime = 0;
unsigned long lastUploadTime = 0;

// --- CURRENT VARIABLES & ARRAY ---
int currentSampleCount = 0;
float currentSum = 0;
float currentHistory[10]; 
int currentHistoryIndex = 0;

// --- MIC VARIABLES & ARRAY ---
long micSampleCount = 0;
double micSum = 0;
double micSumOfSquares = 0;
float micHistory[2];
int micHistoryIndex = 0;

// ==========================================
// 2. GRAFANA/PROMETHEUS CONFIGURATION
// ==========================================
PromLokiTransport transport;
PromClient client(transport);

// Create a WriteRequest for 6 total series. Buffer size increased to 2048 to handle the arrays safely.
WriteRequest req(6, 2048);

// Define all 6 TimeSeries endpoints [cite: 3, 4]
TimeSeries ts_temp(1, "temperature", "{job=\"esp32-test\",host=\"esp32\"}");
TimeSeries ts_humid(1, "humidity", "{job=\"esp32-test\",host=\"esp32\"}");
TimeSeries ts_pressure(1, "pressure", "{job=\"esp32-test\",host=\"esp32\"}");
TimeSeries ts_co(1, "carbon_monoxide", "{job=\"esp32-test\",host=\"esp32\"}");
TimeSeries ts_current(10, "average_current_2_sec", "{job=\"esp32-test\",host=\"esp32\"}");
TimeSeries ts_sound(2, "sound_level", "{job=\"esp32-test\",host=\"esp32\"}");

// ==========================================
// 3. INITIALIZATION
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // Wait 5s for serial connection [cite: 5]
  uint8_t serialTimeout = 0;
  while (!Serial && serialTimeout < 50) { [cite: 6]
      delay(100);
      serialTimeout++;
  }

  Serial.println("--- Starting Grafana-Integrated IoT Node ---");

  // --- HARDWARE SETUP ---
  pinMode(MIC_PIN, INPUT);
  pinMode(CURRENT_PIN, INPUT);
  pinMode(CO_PIN, INPUT);

  if (!bme.begin()) {
    Serial.println("BME680 not found, check wiring!");
    while (1); 
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);

  // --- NETWORK & TRANSPORT SETUP ---
  transport.setUseTls(true); [cite: 8]
  transport.setCerts(grafanaCert, strlen(grafanaCert)); [cite: 9]
  transport.setWifiSsid(WIFI_SSID);
  transport.setWifiPass(WIFI_PASSWORD);
  transport.setDebug(Serial);

  if (!transport.begin()) { [cite: 10]
      Serial.println(transport.errmsg);
      Serial.println("Transport failed. Rebooting in 5 seconds...");
      delay(5000);
      ESP.restart(); 
  }

  // --- CLIENT SETUP ---
  client.setUrl(GC_URL); [cite: 11]
  client.setPath((char*)GC_PATH); [cite: 12]
  client.setPort(GC_PORT);
  client.setUser(GC_USER);
  client.setPass(GC_PASS);
  client.setDebug(Serial);

  if (!client.begin()) {
      Serial.println(client.errmsg);
      Serial.println("Client failed. Rebooting in 5 seconds..."); [cite: 13]
      delay(5000);
      ESP.restart(); 
  }

  // Bundle all TimeSeries into the master WriteRequest
  req.addTimeSeries(ts_temp); [cite: 13]
  req.addTimeSeries(ts_humid);
  req.addTimeSeries(ts_pressure);
  req.addTimeSeries(ts_co);
  req.addTimeSeries(ts_current);
  req.addTimeSeries(ts_sound);
  
  Serial.println("Initialization Complete. Gathering first batch...");
}

// ==========================================
// 4. MAIN LOOP
// ==========================================
void loop() {
  unsigned long currentMillis = millis();

  // ---------------------------------------------------------
  // TIMER 1: CURRENT SENSOR (1 sample every 200ms)
  // ---------------------------------------------------------
  if (currentMillis - lastCurrentSampleTime >= 200) { [cite: 36]
    int currentADC = analogRead(CURRENT_PIN);
    currentSum += (currentADC * mV_PER_STEP);
    currentSampleCount++;
    lastCurrentSampleTime = currentMillis;

    if (currentSampleCount >= 10) { [cite: 37]
      if (currentHistoryIndex < 10) {
        currentHistory[currentHistoryIndex] = currentSum / 10.0;
        currentHistoryIndex++; [cite: 38]
      }
      currentSum = 0;
      currentSampleCount = 0;
    }
  }

  // ---------------------------------------------------------
  // TIMER 2: MICROPHONE SAMPLING (500Hz)
  // ---------------------------------------------------------
  if (currentMillis - lastMicSampleTime >= 2) { [cite: 39]
    int micADC = analogRead(MIC_PIN);
    micSum += micADC;
    micSumOfSquares += ((double)micADC * micADC); [cite: 40]
    micSampleCount++;
    lastMicSampleTime = currentMillis;
  }

  // ---------------------------------------------------------
  // TIMER 3: MICROPHONE RMS CALCULATION (Every 10 seconds)
  // ---------------------------------------------------------
  if (currentMillis - lastMicCalcTime >= 10000) { [cite: 41]
    if (micSampleCount > 0 && micHistoryIndex < 2) {
      double mean = micSum / micSampleCount;
      double meanOfSquares = micSumOfSquares / micSampleCount; [cite: 42]
      double variance = meanOfSquares - (mean * mean);
      if (variance < 0) variance = 0; [cite: 43]
      
      micHistory[micHistoryIndex] = sqrt(variance) * mV_PER_STEP;
      micHistoryIndex++;
    }

    micSum = 0;
    micSumOfSquares = 0; [cite: 44]
    micSampleCount = 0;
    lastMicCalcTime = currentMillis;
  }

  // ---------------------------------------------------------
  // TIMER 4: GRAFANA UPLOAD & PRINT (Every 20 seconds)
  // ---------------------------------------------------------
  if (currentMillis - lastUploadTime >= 20000) {
    
    // 1. Grab instantaneous final readings
    bme.performReading(); [cite: 44]
    int co_raw = analogRead(CO_PIN); [cite: 45]

    // 2. Print local verification to Serial Monitor
    Serial.println("\n===== 20-SECOND PAYLOAD BATCH =====");
    Serial.print("Temperature:  "); Serial.print(bme.temperature); Serial.println(" *C");
    Serial.print("Humidity:     "); Serial.print(bme.humidity); Serial.println(" %"); [cite: 46]
    Serial.print("Pressure:     "); Serial.print(bme.pressure / 100.0); Serial.println(" hPa");
    Serial.print("CO Raw Level: "); Serial.println(co_raw); [cite: 47]
    Serial.print("Current Array (10): [");
    for (int i = 0; i < currentHistoryIndex; i++) { [cite: 48]
      Serial.print(currentHistory[i]);
      if (i < currentHistoryIndex - 1) Serial.print(", "); [cite: 49]
    }
    Serial.println("]");
    Serial.print("Audio Array (2): [");
    for (int i = 0; i < micHistoryIndex; i++) { [cite: 51]
      Serial.print(micHistory[i]);
      if (i < micHistoryIndex - 1) Serial.print(", "); [cite: 52]
    }
    Serial.println("]");

    // 3. GRAFANA UPLOAD PROCESS
    int64_t time = transport.getTimeMillis(); [cite: 14]
    
    if (time == 0) { [cite: 15]
        Serial.println("--> ERROR: NTP Sync Failed. Skipping Grafana upload for this cycle.");
    } else {
        Serial.println("--> ATTEMPTING SEND TO GRAFANA CLOUD...");
        
        // Add single readings
        ts_temp.addSample(time, bme.temperature);
        ts_humid.addSample(time, bme.humidity);
        ts_pressure.addSample(time, bme.pressure / 100.0);
        ts_co.addSample(time, co_raw);

        // Add Current Array (Backdated offset: 20s, 18s, 16s... ago)
        for (int i = 0; i < currentHistoryIndex; i++) {
            int64_t sampleTime = time - (20000 - ((i + 1) * 2000));
            ts_current.addSample(sampleTime, currentHistory[i]);
        }

        // Add Audio Array (Backdated offset: 20s, 10s ago)
        for (int i = 0; i < micHistoryIndex; i++) {
            int64_t sampleTime = time - (20000 - ((i + 1) * 10000));
            ts_sound.addSample(sampleTime, micHistory[i]);
        }

        // Fire the payload
        PromClient::SendResult res = client.send(req); [cite: 21]
        
        if (res == PromClient::SendResult::SUCCESS) {
            Serial.println("--> SUCCESS: Data accepted by Grafana!");
        } else {
            Serial.print("--> FAILED: Error Code: ");
            Serial.println((int)res); [cite: 22]
            Serial.print("--> Message: ");
            Serial.println(client.errmsg); [cite: 23]
        }
        
        // Reset Prometheus sample buffers so they don't double-send next loop [cite: 22]
        ts_temp.resetSamples();
        ts_humid.resetSamples();
        ts_pressure.resetSamples();
        ts_co.resetSamples();
        ts_current.resetSamples();
        ts_sound.resetSamples();
    }
    Serial.println("===================================");

    // 4. Reset local arrays for the next 20 seconds [cite: 53]
    currentHistoryIndex = 0;
    micHistoryIndex = 0;
    
    lastUploadTime = currentMillis; [cite: 54]
  }
}
