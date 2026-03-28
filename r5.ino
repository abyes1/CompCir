#include "config.h"
#include "certificates.h"
#include <PromLokiTransport.h>
#include <PrometheusArduino.h>

PromLokiTransport transport;
PromClient client(transport);

// Create a write request for 1 series (matching the active time series).
WriteRequest req(1, 1024);

// Check out https://prometheus.io/docs/practices/naming/ for metric naming and label conventions.
TimeSeries ts_temp(1, "temperature", "{job=\"esp32-test\",host=\"esp32\"}");

// TimeSeries ts_humid(1, "humidity", "{job=\"esp32-test\",host=\"esp32\",foo=\"bar\"}");
// TimeSeries ts_pressure(1, "pressure", "{job=\"esp32-test\",host=\"esp32\"}");
// TimeSeries ts_avg_curr(10, "average_current_2_sec", "{job=\"esp32-test\",host=\"esp32\"}");
// TimeSeries ts_co_meas(20, "carbon_monoxide", "{job=\"esp32-test\",host=\"esp32\"}");
// TimeSeries ts_sound(2, "sound_level", "{job=\"esp32-test\",host=\"esp32\"}");

int loopCounter = 0;

void setup() {

    Serial.begin(115200);

    // Wait 5s for serial connection or continue without it
    // Initialized to 0 to prevent garbage memory bugs
    uint8_t serialTimeout = 0; 
    while (!Serial && serialTimeout < 50) {
        delay(100);
        serialTimeout++;
    }

    Serial.println("Starting");
    Serial.print("Free Mem Before Setup: ");
    Serial.println(freeMemory());

    // Configure and start the transport layer
    transport.setUseTls(true); // MUST be true for HTTPS port 443
    transport.setCerts(grafanaCert, strlen(grafanaCert));
    transport.setWifiSsid(WIFI_SSID);
    transport.setWifiPass(WIFI_PASSWORD);
    transport.setDebug(Serial);

    if (!transport.begin()) {
        Serial.println(transport.errmsg);
        Serial.println("Transport failed. Rebooting in 5 seconds...");
        delay(5000);
        ESP.restart(); // Prevents the board from freezing on a network drop
    }

    // Configure the client
    client.setUrl(GC_URL);
    client.setPath((char*)GC_PATH);
    client.setPort(GC_PORT);
    client.setUser(GC_USER);
    client.setPass(GC_PASS);
    client.setDebug(Serial);

    if (!client.begin()) {
        Serial.println(client.errmsg);
        Serial.println("Client failed. Rebooting in 5 seconds...");
        delay(5000);
        ESP.restart(); // Prevents the board from freezing
    }

    // Add our TimeSeries to the WriteRequest
    req.addTimeSeries(ts_temp);

    req.setDebug(Serial);  

    Serial.print("Free Mem After Setup: ");
    Serial.println(freeMemory());

};


void loop() {
    int64_t time;
    time = transport.getTimeMillis();

    // DEBUG: Check if time sync is working
    if (time == 0) {
        Serial.println("DEBUG: Time is 0! NTP sync might have failed. Grafana will reject 0 timestamps.");
    } else {
        Serial.print("DEBUG: Current Sync Time: ");
        Serial.println(time);
    }

    // Add sample
    if (!ts_temp.addSample(time, millis())) {
        Serial.print("DEBUG Sample Error: ");
        Serial.println(ts_temp.errmsg);
    } else {
        Serial.println("DEBUG: Sample added to buffer.");
    }

    loopCounter++;
    Serial.print("DEBUG: loopCounter at ");
    Serial.println(loopCounter);

    if (loopCounter >= 4) {
        Serial.println(">>> ATTEMPTING SEND TO GRAFANA <<<");
        loopCounter = 0;
        
        PromClient::SendResult res = client.send(req);
        
        if (res == PromClient::SendResult::SUCCESS) {
            Serial.println("SUCCESS: Data accepted by Grafana Cloud!");
            ts_temp.resetSamples();
        } else {
            Serial.print("FAILED: Error Code: ");
            Serial.println((int)res);
            Serial.print("Error Message: ");
            Serial.println(client.errmsg);
            
            // Helpful hints for specific codes
            if ((int)res == 401) Serial.println("HINT: 401 = Check your API Key (Password).");
            if ((int)res == 403) Serial.println("HINT: 403 = Check your User ID or URL.");
            if ((int)res == 400) Serial.println("HINT: 400 = Likely out-of-order timestamps or bad labels.");
        }
    }

    Serial.println("-------------------------");
    delay(20000);
};
