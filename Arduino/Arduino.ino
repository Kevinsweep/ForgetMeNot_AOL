#include <WiFi.h>
#include <HTTPClient.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "spo2_algorithm.h"
#include "config.h"
#include <TinyGPS++.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// constants
const char* SSID = WIFI_SSID;
const char* PASSWORD = WIFI_PASS;

int http_response_code;
MAX30105 sensor_hr;

// max30102 settings
// hr (beat detection — reliable pulse timing; HR comes from here, not the Maxim algo)
const byte RATE_SIZE = 4;   // How many beats we average
byte rates[RATE_SIZE];      // Store last few valid BPM values
byte rate_spot = 0;
byte valid_beats = 0;
long last_beat = 0;
volatile float current_BPM = 0;
volatile int average_BPM = 0;
const float MIN_BPM = 20;
const float MAX_BPM = 255;

// spo2 (Maxim built-in algorithm: used ONLY for SpO2; its HR output is ignored)
#define BUFFER_SIZE 100
uint32_t ir_buffer[BUFFER_SIZE];
uint32_t red_buffer[BUFFER_SIZE];
int32_t spo2;            // Oxygen saturation value
int8_t  valid_SPO2;      // 1 = valid reading, 0 = not valid yet

// DS18B20 settings
#define BODY_TEMP_PIN 4
OneWire oneWire(BODY_TEMP_PIN);
DallasTemperature sensor_body_temp(&oneWire);

// Neo6m settings
#define RXD2 16
#define TXD2 17

#define GPS_BAUD 9600

// The TinyGPS++ object
TinyGPSPlus gps;

// Create an instance of the HardwareSerial class for Serial 2
HardwareSerial gpsSerial(2);

// actuator settings
#define SERVO_PIN 13
#define BUZZER_PIN 19

Servo emergency_signal;

// to send
volatile double body_temp_to_send = 0;
volatile int heart_rate_to_send = 0;
double spo2_to_send = 0;
double latitude;
double longitude;
double speed;

const unsigned long upload_API_interval = 1000;
const unsigned long predict_API_interval = 5000;
const unsigned long serial_interval = 2000;
unsigned long acc2_interval = 1000;
const unsigned long beep_duration = 200;   // how long each beep stays on (ms)
unsigned long previous_upload_API_millis = 0;
unsigned long previous_predict_API_millis = 0;
unsigned long previous_serial_millis = 0;
unsigned long previous_acc2_millis = 0;
bool emergency_signal_state = false;
int current_risk = 0;

// taskhandle
TaskHandle_t monitoring;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  // connect WIFI
  WiFi.begin(SSID, PASSWORD);
  Serial.println("Connecting");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());
  // sensor init
  sensor_hr.begin(Wire, I2C_SPEED_FAST);
  sensor_body_temp.begin();
  
  sensor_hr.setup();

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);

  pinMode(BUZZER_PIN, OUTPUT);
  emergency_signal.attach(SERVO_PIN);

  // use core 0 for monitoring
  xTaskCreatePinnedToCore(
    monitor_sensors,       /* Task function. */
    "monitor_sensors",     /* name of task. */
    10000,                 /* Stack size of task */
    NULL,                  /* parameter of the task */
    0,                     /* priority of the task */
    &monitoring,           /* Task handle to keep track of created task */
    0                      /* pin task to core 0 */     
  );

  delay(500); 
}


void buzzer_beep(bool flag){
  digitalWrite(BUZZER_PIN, flag);
}

void emergency_signal_up(bool flag){
  if (flag){
    emergency_signal.write(90);
  }
  else{
    emergency_signal.write(0);
  }
}

double read_body_temp(){
  sensor_body_temp.requestTemperatures(); 
  double temperatureC = sensor_body_temp.getTempCByIndex(0);
  return temperatureC;
}

// Note: Depending on your MCU (AVR vs ARM), change uint32_t pointers to uint16_t if memory is constrained.
void calculate_spo2(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint32_t *pun_red_buffer, int32_t *pn_spo2, int8_t *pch_spo2_valid) 
{
  uint32_t un_ir_mean;
  int32_t k, i, n_i_ratio_count, n_exact_ir_valley_locs_count, n_middle_idx;
  int32_t n_th1, n_npks;   
  int32_t an_ir_valley_locs[15];
  
  int32_t n_y_ac, n_x_ac;
  int32_t n_spo2_calc; 
  int32_t n_y_dc_max, n_x_dc_max; 
  int32_t n_y_dc_max_idx = 0;
  int32_t n_x_dc_max_idx = 0; 
  int32_t an_ratio[5], n_ratio_average; 
  int32_t n_nume, n_denom;

  // Local buffers for processing (Make these global if you are low on stack memory/SRAM)
  int32_t an_x[BUFFER_SIZE];
  int32_t an_y[BUFFER_SIZE];

  // 1. Calculate DC mean and subtract DC from IR
  un_ir_mean = 0; 
  for (k = 0; k < n_ir_buffer_length; k++) un_ir_mean += pun_ir_buffer[k];
  un_ir_mean = un_ir_mean / n_ir_buffer_length;
    
  // 2. Remove DC and invert signal so we can use the peak detector as a valley detector
  for (k = 0; k < n_ir_buffer_length; k++)  
    an_x[k] = -1 * (pun_ir_buffer[k] - un_ir_mean); 
    
  // 3. 4-point Moving Average
  for (k = 0; k < BUFFER_SIZE - MA4_SIZE; k++) {
    an_x[k] = (an_x[k] + an_x[k+1] + an_x[k+2] + an_x[k+3]) / 4;        
  }

  // 4. Calculate threshold  
  n_th1 = 0; 
  for (k = 0; k < BUFFER_SIZE; k++) {
    n_th1 += an_x[k];
  }
  n_th1 = n_th1 / BUFFER_SIZE;
  if (n_th1 < 30) n_th1 = 30; // min allowed
  if (n_th1 > 60) n_th1 = 60; // max allowed

  for (k = 0; k < 15; k++) an_ir_valley_locs[k] = 0;
  
  // 5. Find valleys (using peak detector on inverted signal)
  maxim_find_peaks(an_ir_valley_locs, &n_npks, an_x, BUFFER_SIZE, n_th1, 4, 15); 

  // 6. Load raw values again for SPO2 calculation: RED(=y) and IR(=x)
  for (k = 0; k < n_ir_buffer_length; k++) {
      an_x[k] = pun_ir_buffer[k]; 
      an_y[k] = pun_red_buffer[k]; 
  }

  n_exact_ir_valley_locs_count = n_npks; 
  n_ratio_average = 0; 
  n_i_ratio_count = 0; 
  
  for(k = 0; k < 5; k++) an_ratio[k] = 0;
  
  // Safety check on valley locations
  for (k = 0; k < n_exact_ir_valley_locs_count; k++) {
    if (an_ir_valley_locs[k] > BUFFER_SIZE) {
      *pn_spo2 = -999; // do not use SPO2 since valley loc is out of range
      *pch_spo2_valid = 0; 
      return;
    }
  }

  // 7. Find max between two valley locations and calculate AC/DC ratio
  for (k = 0; k < n_exact_ir_valley_locs_count - 1; k++) {
    n_y_dc_max = -16777216; 
    n_x_dc_max = -16777216; 
    
    if (an_ir_valley_locs[k+1] - an_ir_valley_locs[k] > 3) {
        for (i = an_ir_valley_locs[k]; i < an_ir_valley_locs[k+1]; i++) {
          if (an_x[i] > n_x_dc_max) { n_x_dc_max = an_x[i]; n_x_dc_max_idx = i; }
          if (an_y[i] > n_y_dc_max) { n_y_dc_max = an_y[i]; n_y_dc_max_idx = i; }
      }
      
      n_y_ac = (an_y[an_ir_valley_locs[k+1]] - an_y[an_ir_valley_locs[k]]) * (n_y_dc_max_idx - an_ir_valley_locs[k]); 
      n_y_ac = an_y[an_ir_valley_locs[k]] + n_y_ac / (an_ir_valley_locs[k+1] - an_ir_valley_locs[k]); 
      n_y_ac = an_y[n_y_dc_max_idx] - n_y_ac;    
      
      n_x_ac = (an_x[an_ir_valley_locs[k+1]] - an_x[an_ir_valley_locs[k]]) * (n_x_dc_max_idx - an_ir_valley_locs[k]); 
      n_x_ac = an_x[an_ir_valley_locs[k]] + n_x_ac / (an_ir_valley_locs[k+1] - an_ir_valley_locs[k]); 
      n_x_ac = an_x[n_y_dc_max_idx] - n_x_ac;      
      
      n_nume = (n_y_ac * n_x_dc_max) >> 7; 
      n_denom = (n_x_ac * n_y_dc_max) >> 7;
      
      if (n_denom > 0 && n_i_ratio_count < 5 && n_nume != 0) {   
        an_ratio[n_i_ratio_count] = (n_nume * 100) / n_denom; 
        n_i_ratio_count++;
      }
    }
  }
  
  // 8. Choose median value
  maxim_sort_ascend(an_ratio, n_i_ratio_count);
  n_middle_idx = n_i_ratio_count / 2;

  if (n_middle_idx > 1)
    n_ratio_average = (an_ratio[n_middle_idx-1] + an_ratio[n_middle_idx]) / 2; // use median
  else
    n_ratio_average = an_ratio[n_middle_idx];

  // 9. Look up SpO2 in table
  if (n_ratio_average > 2 && n_ratio_average < 184) {
    n_spo2_calc = uch_spo2_table[n_ratio_average];
    *pn_spo2 = n_spo2_calc;
    *pch_spo2_valid = 1;
  }
  else {
    *pn_spo2 = -999; 
    *pch_spo2_valid = 0; 
  }
}

void monitor_sensors(void * pvParameters){
  int cur_idx = 0;
  for(;;){
    long ir_value = sensor_hr.getIR();
    long red_value = sensor_hr.getRed();
    sensor_hr.nextSample();
    ir_buffer[cur_idx] = ir_value;
    red_buffer[cur_idx] = red_value;
    cur_idx++;
    cur_idx %= BUFFER_SIZE;
    if (checkForBeat(ir_value) == true){
      long time_between_beats = millis() - last_beat;
      last_beat = millis();

      current_BPM = 60.0 / (time_between_beats / 1000.0);

      // keep realistic values
      if (current_BPM >= MIN_BPM && current_BPM <= MAX_BPM){
        rates[rate_spot] = (byte)current_BPM;
        rate_spot++;
        rate_spot %= RATE_SIZE;

        average_BPM = 0;
        for (byte i = 0; i < RATE_SIZE; i++) {
          average_BPM += rates[i];
        }
        average_BPM /= RATE_SIZE;

        heart_rate_to_send = average_BPM;
      }
      calculate_spo2(ir_buffer, BUFFER_SIZE, red_buffer, &spo2, &valid_SPO2);
      spo2_to_send = spo2;
    }
  }
}

bool value_sanity_check(double temp, int rate, double spo2){
  if(temp < -100 || rate <= 0 || spo2 <= 0){
    return false;
  }
  else if(latitude == 0 && longitude == 0){
    return false;
  }
  return true;
}

void get_location_info(){
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
  if (gps.location.isUpdated()) {
    latitude = gps.location.lat();
    longitude = gps.location.lng();
    speed = gps.speed.kmph();
  }
}

void call_upload_API(){
  WiFiClient client;
  HTTPClient http;

  // Your Domain name with URL path or IP address with path
  http.begin(client, UPLOAD_URL);
  http.addHeader("Content-Type", "application/json");
  char post_string[256];
  sprintf(post_string, "{\"body_temp\":%.2lf, \"heart_rate\":%d, \"spo2\":%.2lf, \"latitude\":%lf, \"longitude\":%lf, \"speed\":%.2lf}",body_temp_to_send, heart_rate_to_send, spo2_to_send, latitude, longitude, speed);
  http_response_code = http.POST(post_string);
  Serial.print("HTTP Response code (upload): ");
  Serial.println(http_response_code);
  Serial.println(post_string);
  http.end();
}

void call_predict_API(){
  WiFiClient client;
  HTTPClient http;

  http.begin(client, PREDICT_URL);
  http_response_code = http.GET();

  if (http_response_code == 200) {
    String payload = http.getString();

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      const char* risk = doc["risk_level"] | "";

      // store in a global the actuator logic reads
      if (strcmp(risk, "Danger")  == 0)
        current_risk = 2;
      else if (strcmp(risk, "Warning") == 0)
        current_risk = 1;
      else
        current_risk = 0;
    }
  }
  Serial.print("HTTP Response code (predict): ");
  Serial.println(http_response_code);
  http.end();
}

void loop() {
  // put your main code here, to run repeatedly:
  unsigned long current_millis = millis(); 

  body_temp_to_send = read_body_temp();
  get_location_info();

  // Print to serial monitor
  if (current_millis - previous_serial_millis >= serial_interval){
    previous_serial_millis = current_millis;

    Serial.println(body_temp_to_send);
    Serial.println(heart_rate_to_send);
    Serial.println(spo2_to_send);
    Serial.print("BPM=");
    Serial.println(current_BPM);
    Serial.print("Avg BPM=");
    Serial.println(average_BPM);
  }

  // call API
  if (current_millis - previous_upload_API_millis >= upload_API_interval && value_sanity_check(body_temp_to_send, heart_rate_to_send, spo2_to_send)) {
    previous_upload_API_millis = current_millis; 
    call_upload_API();
  }

  // acctuator logic (rule based automation)
  if(current_millis - previous_predict_API_millis > predict_API_interval){
    previous_predict_API_millis = current_millis; 
    call_predict_API();
    if (current_risk == 2){
      acc2_interval = 500;
      emergency_signal_state = true;
      Serial.print("Prediction = Danger");

    }
    else if (current_risk == 1){
      acc2_interval = 1000;
      emergency_signal_state = true;
      Serial.print("Prediction = Warning");
    }
    else{
      acc2_interval = 1000;
      emergency_signal_state = false;
      Serial.print("Prediction = Normal");
    }
  }

  if(current_millis - previous_acc2_millis > acc2_interval){
    previous_acc2_millis = current_millis;
    emergency_signal_up(emergency_signal_state);
    if (emergency_signal_state){
      buzzer_beep(true);
      delay(beep_duration);
      buzzer_beep(false);
    }
    else{
      buzzer_beep(false);
    }
  }
}
