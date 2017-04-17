// 指令转换函数，输入类型，DATA-ID，以及DATA，转换为曼切斯特波32位数组
uint32_t calculate(const uint8_t type, const uint8_t id,const uint16_t data){
  uint8_t parity=0,i=0;
  uint32_t tmp=0;
  tmp=((uint32_t)type<<28) + ((uint32_t)id<<16) + data;
  for(i=0;i<31;i++) {
    parity^=(tmp&bit(i)?1:0);
  }
  tmp|=parity?bit(31):0;
  return tmp;
}

#define S_FAULT          1
#define S_CH_ACTIVE      2
#define S_DHW_ACTIVE     4
#define S_FLAME          8
#define S_COMF_ACTIVE  512
#define F_SERVICE        1
#define F_LOW_WATER      4
#define F_GAS            8
#define F_AIR_PRESSURE  10
#define F_WATER_OV_TEMP 32

int OT_IN_PIN = D5;
int OT_OUT_PIN = D6;

byte req_idx = 0;
int state = 0;
unsigned long time_stamp;
char message[150];

//P MGS-TYPE SPARE DATA-ID  DATA-VALUE
//0 000      0000  00000000 00000000 00000000
//详细内容请阅读OpenTherm2.2协议规范文档
unsigned long requests[] = {
  calculate(0, 0, 3 << 8), // 获取壁挂炉状态，同时设定壁挂炉运行模式 - 允许采暖, 允许生活水
  calculate(0, 5, 0), // 获取壁挂炉故障信息
  calculate(1, 1, 35 << 8),// 设置采暖水温度 35C
  calculate(1, 56, 40 << 8),// 设置生活水温度 40C
  calculate(0, 25, 0), // 获取壁挂炉温度
};

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
      if (dataValue & S_FAULT) Serial.println("Failure"); // 故障
      break;
    case 5: // 故障查询回应
      switch(dataValue >> 8 & 0x3D) {
        case F_SERVICE:
          Serial.println("Service Needed");
          break;
        case F_LOW_WATER:
          Serial.println("Low Water Pressure");
          break;
        case F_GAS:
          Serial.println("Now Gas");
          break;
        case F_AIR_PRESSURE:
          Serial.println("Air Pressure Failure");
          break;
        case F_WATER_OV_TEMP:
          Serial.println("Water Over Temperature");
          break;
      }
      break;
  }
  state = 0;
}

void setup() {
  pinMode(OT_IN_PIN, INPUT);
  pinMode(OT_OUT_PIN, OUTPUT);
  Serial.begin(115200);
  Serial.println("Start");
  activateBoiler();
}

void loop() {
  switch(state){
    case 0:
      sendRequest(requests[req_idx]);
      break;
    case 1:
      waitForResponse();
      break;
    case 2:
      readResponse();
      req_idx++;
      if (req_idx >= sizeof(requests)/sizeof(unsigned long)) {
        req_idx = 0;
      }
      delay(950);
      break;
  }
}
