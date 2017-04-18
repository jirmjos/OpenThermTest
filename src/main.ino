#include <ESP8266WiFi.h> // Wifi支持
#include <PubSubClient.h> // MQTT协议支持

#define WIFI_SSID "AIRFIT_WIRELESS"
#define WIFI_PASSWORD "XXXXXXXXXXXX"
#define MQTT_SERVER "192.168.1.148"
#define MQTT_PORT 1883
#define MQTT_TOKEN "mosquitto"
#define DEVICE_ID "BOILER"
#define SUB_MODE "ot/set_mode"
#define SUB_CH_TEMP "ot/set_ch_temp"
#define SUB_DHW_TEMP "ot/set_dhw_temp"
#define PUB_ERR_CODE "ot/err_code"
#define PUB_CH_TEMP "ot/ch_temp"


WiFiClient espClient;
PubSubClient mqttClient(espClient);

uint8_t targetMode = 3;
uint8_t targetCHTemp = 45, targetDHWTemp = 45;
bool changedCHTemp = false, changedDHWTemp = false, changedMode = false, errRaised = false, checkBoilerTemp = false;
char message[20];

#define S_FAULT          1  // 故障
#define S_CH_ACTIVE      2  // 采暖模式
#define S_DHW_ACTIVE     4  // 生活水模式
#define S_FLAME          8  // 燃烧
#define S_COMF_ACTIVE  512  // 舒适模式
#define F_SERVICE        1  // 需服务
#define F_LOW_WATER      4  // 系统水压不足
#define F_GAS            8  // 没有燃气
#define F_AIR_PRESSURE  16  // 风压故障
#define F_WATER_OV_TEMP 32  // 超温故障

int OT_IN_PIN = D5;
int OT_OUT_PIN = D6;

byte req_idx = 0;
int state = 0;
unsigned long time_stamp;

// MQTT 收到订阅信息时响应
void mqttCallBack(char * topic, byte* payload, uint8_t length) {
  uint8_t same = strcmp(topic, SUB_CH_TEMP); // 是不是温度调整
  if(same == 0){
    String a = "";
    for(uint8_t i = 0; i < length; i++) {
      a = a + (char)payload[i];
    }
    targetCHTemp = atoi(a.c_str());
    changedCHTemp = true;
  }
  same = strcmp(topic, SUB_DHW_TEMP); // 是不是温度调整
  if(same == 0){
    String a = "";
    for(uint8_t i = 0; i < length; i++) {
      a = a + (char)payload[i];
    }
    targetDHWTemp = atoi(a.c_str());
    changedDHWTemp = true;
  }
  same = strcmp(topic, SUB_MODE); // 是不是模式调整
  if(same == 0){
    targetMode = (int)payload[0]-48;
    changedMode = true;
  }
}

// 连接MQTT Broker
void mqttReconnect() {
  if(!mqttClient.connected()) {
    if(mqttClient.connect(DEVICE_ID, MQTT_TOKEN, "password")) {
      Serial.println("MQTT connected");
      mqttClient.subscribe(SUB_MODE);
      mqttClient.subscribe(SUB_CH_TEMP);
      mqttClient.subscribe(SUB_DHW_TEMP);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println("try again in 5 seconds");
    }
  }
}

// 指令转换函数，输入类型，DATA-ID，以及DATA，转换为曼切斯特波32位数组
//P MGS-TYPE SPARE DATA-ID  DATA-VALUE
//0 000      0000  00000000 00000000 00000000
//  calculate(0, 0, 3 << 8), // 获取壁挂炉状态，同时设定壁挂炉运行模式 - 允许采暖, 允许生活水
//  calculate(0, 5, 0), // 获取壁挂炉故障信息
//  calculate(1, 1, 45 << 8),// 设置采暖水温度
//  calculate(1, 56, 45 << 8),// 设置生活水温度
//  calculate(0, 25, 0), // 获取壁挂炉温度
uint32_t calculate(const uint8_t type, const uint8_t id, const uint16_t data){
  uint8_t parity=0, i=0;
  uint32_t tmp=0;
  tmp = ((uint32_t)type << 28) + ((uint32_t)id << 16) + data;
  for(i=0; i<31; i++) {
    parity ^= (tmp & bit(i)?1:0);
  }
  tmp |= parity ? bit(31):0;
  return tmp;
}

void setIdleState() {
  digitalWrite(OT_OUT_PIN, HIGH);
}

void setActiveState() {
  digitalWrite(OT_OUT_PIN, LOW);
}

void activateBoiler() {
  setIdleState();
  delay(1000);
}

void sendBit(bool high) {
  if (high) setActiveState(); else setIdleState();
  delayMicroseconds(500);
  if (high) setIdleState(); else setActiveState();
  delayMicroseconds(500);
}

void sendFrame(unsigned long request) {
  sendBit(HIGH); //start bit
  for (int i = 31; i >= 0; i--) {
    sendBit(bitRead(request, i));
  }
  sendBit(HIGH); //stop bit
  setIdleState();
}

void printBinary(unsigned long val) {
  for (int i = 31; i >= 0; i--) {
    Serial.print(bitRead(val, i));
  }
}

void sendRequest(unsigned long request) {
  Serial.println();
  Serial.print("Request:  ");
  printBinary(request);
  Serial.print(" / ");
  Serial.print(request, HEX);
  Serial.println();
  sendFrame(request);
  time_stamp = millis();
  state = 1;
}

void waitForResponse() {
  if (digitalRead(OT_IN_PIN) == HIGH) { // 起始位
    delayMicroseconds(1005); //1ms -10%+15%
    state = 2;
  } else if (millis() - time_stamp >= 1000) {
    Serial.println("Response timeout");
    state = 0;
  }
}

void readResponse() {
  unsigned long response = 0;
  for (int i = 0; i < 32; i++) {
    response = (response << 1) | digitalRead(OT_IN_PIN);
    delayMicroseconds(1005); //1ms -10%+15%
  }
  Serial.print("Response: ");
  printBinary(response);
  Serial.print(" / ");
  Serial.print(response, HEX);
  Serial.println();

  uint8_t head = response >> 16 & 0xFF;
  uint16_t dataValue = (uint16_t)response;
  switch(head){
    case 25: // 采暖水温度
      Serial.print("Temperature = ");
      Serial.print(dataValue >> 8 & 0xFF);
      Serial.println("");
      sprintf(message, "%d", dataValue >> 8 & 0xFF);
      mqttClient.publish(PUB_CH_TEMP, message);
      break;
    case 0: // 状态查询回应
      if (dataValue & S_DHW_ACTIVE) { // 生活水状态
        Serial.println("Domestic How Water");
      } else if (dataValue & S_CH_ACTIVE) { // 采暖状态
        Serial.println("Central Heating");
      } else {
        Serial.println("Idle");
      }
      if (dataValue & S_FLAME) Serial.println("Flaming"); // 燃烧状态
      if (dataValue & S_COMF_ACTIVE) Serial.println("Comfort");  // 舒适模式
      if (dataValue & S_FAULT) {
        Serial.println("Failure"); // 故障
        errRaised = true;
      }
      break;
    case 5: // 故障查询回应
      switch(dataValue >> 8 & 0x3D) {
        case F_SERVICE:
          Serial.println("Service Needed");
          sprintf(message, "5");
          break;
        case F_LOW_WATER:
          Serial.println("Low Water Pressure");
          sprintf(message, "2");
          break;
        case F_GAS:
          Serial.println("No Gas");
          sprintf(message, "1");
          break;
        case F_AIR_PRESSURE:
          Serial.println("Air Pressure Failure");
          sprintf(message, "3");
          break;
        case F_WATER_OV_TEMP:
          Serial.println("Water Over Temperature");
          sprintf(message, "4");
          break;
      }
      mqttClient.publish(PUB_ERR_CODE, message);
      sprintf(message,"");
      break;
  }
  state = 0;
}

void setup_wifi() {

  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  Serial.println("Start");
  setup_wifi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallBack);
  pinMode(OT_IN_PIN, INPUT);
  pinMode(OT_OUT_PIN, OUTPUT);
  activateBoiler();
}

void loop() {
  // 处理MQTT Broker连接问题
  if(!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();

  switch(state){
    case 0:
      if(errRaised) {
        sendRequest(calculate(0, 5, 0));
        errRaised = false;
      }else if(changedCHTemp) {
        sendRequest(calculate(1, 1, targetCHTemp << 8 ));
        Serial.print("CH Temp:");
        Serial.println(targetCHTemp);
        changedCHTemp = false;
      } else if(changedDHWTemp) {
        sendRequest(calculate(1, 56, targetDHWTemp << 8));
        Serial.print("DHW Temp:");
        Serial.println(targetDHWTemp);
        changedDHWTemp = false;
      } else if(checkBoilerTemp){
        sendRequest(calculate(0, 25, 0));
        checkBoilerTemp = false;
      }else {
        sendRequest(calculate(0, 0, targetMode << 8));
        changedMode = false;
        if(targetMode & 2) checkBoilerTemp = true;
      }
      break;
    case 1:
      waitForResponse();
      break;
    case 2:
      readResponse();
      delay(950);
      break;
  }
}
