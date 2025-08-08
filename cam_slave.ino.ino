// ========== ESP32-CAM MODULE (CAMERA SLAVE) ==========
// 功能：
// - 接收主控 ESP32 的 CAPTURE_HIGH / CAPTURE_LOW 指令（通过 ESP-NOW）
// - 拍照并分析图像（是否断裂）
// - CAPTURE_HIGH：保存图像到 SD 卡并回复 PHOTO_DONE
// - CAPTURE_LOW：仅分析图像，如异常发送 BROKEN，否则回复 PHOTO_DONE
// - 看门狗复体机制（10秒）

#include <esp_camera.h>
#include <WiFi.h>
#include <esp_now.h>
#include <FS.h>
#include <SD_MMC.h>
#include <time.h>
#include "esp_task_wdt.h"

#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

uint8_t masterAddress[] = { 0x24, 0x6F, 0x28, 0x12, 0x34, 0x56 };  // 替换为主控 ESP32 的 MAC

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "no_time";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &timeinfo);
  return String(buf);
}

void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;
  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed"); while (true);
  }
}

void setupSD() {
  if (!SD_MMC.begin()) {
    Serial.println("SD Card init failed"); while (true);
  }
  Serial.println("SD Card ready");
}

bool isImageAbnormal(camera_fb_t *fb) {
  return fb->len < 10000;
}

void saveImageToSD(camera_fb_t *fb) {
  String filename = "/img_" + getTimeString() + ".jpg";
  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open SD file for writing");
    return;
  }
  file.write(fb->buf, fb->len);
  file.close();
  Serial.println("Saved: " + filename);
}

void sendReply(const char *msg) {
  esp_now_send(masterAddress, (uint8_t *)msg, strlen(msg));
}

void handleCommand(String cmd) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Camera fail"); esp_restart(); return; }
  bool abnormal = isImageAbnormal(fb);
  if (cmd == "CAPTURE_HIGH") {
    saveImageToSD(fb);
    sendReply("PHOTO_DONE");
  } else if (cmd == "CAPTURE_LOW") {
    if (abnormal) sendReply("BROKEN");
    else sendReply("PHOTO_DONE");
  }
  esp_camera_fb_return(fb);
}

void onReceive(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  String msg = "";
  for (int i = 0; i < len; i++) msg += (char)data[i];
  msg.trim();
  Serial.println("CMD from master: " + msg);
  handleCommand(msg);
}

void setupTime() {
  configTime(9 * 3600, 0, "ntp.jst.mfeed.ad.jp", "ntp.nict.jp");
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 20) {
    delay(500); retries++;
  }
  if (retries >= 20) {
    Serial.println("NTP sync failed");
  } else {
    Serial.println("Time synced");
  }
}

void setupESPNOW() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init fail"); while (true); }
  esp_now_register_recv_cb(onReceive);
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0; peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void setup() {
  Serial.begin(115200);
  setupCamera();
  setupSD();
  setupTime();
  setupESPNOW();

  const esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 10000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  Serial.println("ESP32-CAM ready.");
}

void loop() {
  esp_task_wdt_reset();
  delay(100);
}
