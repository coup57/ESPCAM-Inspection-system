// 主控 ESP32（含摄像头）程序
// 功能：动作输出控制、ESP-NOW 通信、图像保存与判断、断裂判断后停止输出并保存记录

#include <esp_now.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <SD_MMC.h>
#include <EEPROM.h>
#include <FS.h>
#include <time.h>

// 摄像头引脚定义（AI Thinker）
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define ACTION_GPIO       14   // 动作输出 GPIO（未占用）
#define START_INPUT_GPIO  33   // 程序启动输入 GPIO（未占用）
#define LED_GPIO           4   // 白色 LED 闪光灯

#define EEPROM_SIZE 4
uint32_t actionCount = 0;
unsigned long lastSaveTime = 0;

bool receivedResult = false;
bool resultOK = true;  // true: OK，false: Fail

uint8_t peerMac[] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF}; // 子机 MAC 地址

void saveLogTXT(const String &prefix) {
  time_t now;
  struct tm timeinfo;
  char filename[64];

  time(&now);
  localtime_r(&now, &timeinfo);
  strftime(filename, sizeof(filename), "/%Y%m%d_%H%M%S.txt", &timeinfo);

  File log = SD_MMC.open(filename, FILE_WRITE);
  if (log) {
    log.printf("%s\n动作次数: %lu\n", prefix.c_str(), actionCount);
    log.close();
  }
}

bool captureAndSave(String &filename) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;

  time_t now;
  struct tm timeinfo;
  char filepath[64];
  time(&now);
  localtime_r(&now, &timeinfo);
  strftime(filepath, sizeof(filepath), "/IMG_%Y%m%d_%H%M%S.jpg", &timeinfo);

  File file = SD_MMC.open(filepath, FILE_WRITE);
  if (!file) return false;
  file.write(fb->buf, fb->len);
  file.close();

  filename = filepath;
  esp_camera_fb_return(fb);
  return true;
}

bool analyzeImage(const String &filename) {
  // 简化处理：图像存在即视为OK（用户可替换图像分析算法）
  return SD_MMC.exists(filename);
}

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           recvInfo->src_addr[0], recvInfo->src_addr[1], recvInfo->src_addr[2],
           recvInfo->src_addr[3], recvInfo->src_addr[4], recvInfo->src_addr[5]);
  Serial.printf("Received message from %s\n", macStr);

  // 你的数据处理逻辑...
}

void sendCaptureCommand() {
  const char cmd = 'C';
  esp_now_send(peerMac, (uint8_t *)&cmd, sizeof(cmd));
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_camera_init(&config);
}

void setup() {
  //WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  pinMode(ACTION_GPIO, OUTPUT);
  pinMode(START_INPUT_GPIO, INPUT_PULLUP);
  pinMode(LED_GPIO, OUTPUT);
  digitalWrite(ACTION_GPIO, LOW);

  initCamera();
  SD_MMC.begin();
  EEPROM.begin(EEPROM_SIZE);
  actionCount = EEPROM.readUInt(0);

  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  configTime(9 * 3600, 0, "pool.ntp.org"); // 设置日本时区
  lastSaveTime = millis();
}

void loop() {
  if (digitalRead(START_INPUT_GPIO) == LOW) {
    digitalWrite(ACTION_GPIO, HIGH);
    delay(200);

    String fname;
    if (!captureAndSave(fname)) return;

    sendCaptureCommand();
    delay(500);

    bool localOK = analyzeImage(fname);
    int wait = 0;
    while (!receivedResult && wait < 2000) {
      delay(100);
      wait += 100;
    }

    if (!localOK || !resultOK) {
      digitalWrite(ACTION_GPIO, LOW);
      saveLogTXT("断裂检测");
      return;
    }

    actionCount++;
    EEPROM.writeUInt(0, actionCount);
    EEPROM.commit();
  }

  if (millis() - lastSaveTime >= 9 * 60 * 60 * 1000UL) {
    String dummy;
    captureAndSave(dummy);
    saveLogTXT("定时记录");
    lastSaveTime = millis();
  }
}
