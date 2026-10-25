#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "secrets.h"

// ==================================================
// XIAO ESP32S3 Sense camera pins
// ==================================================

// Kaameral ei ole eraldi power-down/reset GPIO-d
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1

// Camera clock
#define XCLK_GPIO_NUM 10

// Camera control bus
#define SIOD_GPIO_NUM 40
#define SIOC_GPIO_NUM 39

// Camera image data pins
#define Y9_GPIO_NUM 48
#define Y8_GPIO_NUM 11
#define Y7_GPIO_NUM 12
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 16
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 17
#define Y2_GPIO_NUM 15

// Camera timing pins
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM 47
#define PCLK_GPIO_NUM 13

// ==================================================
// HTTP server
// ==================================================

WebServer server(80);

// ==================================================
// Simple HTML page
// ==================================================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>XIAO Tool Camera</title>

    <style>
        body {
            font-family: Arial, sans-serif;
            text-align: center;
            margin: 20px;
            background: #111;
            color: white;
        }

        img {
            max-width: 100%;
            border: 1px solid #555;
        }
    </style>
</head>

<body>

    <h1>XIAO Tool Camera</h1>

    <img src="/stream">

</body>
</html>
)rawliteral";

// ==================================================
// Camera initialization
// ==================================================

bool initCamera() {
    Serial.println();
    Serial.println("===== CAMERA INIT =====");
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
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;

    // Brauserisse streamimiseks sobib JPEG kõige paremini
    config.pixel_format = PIXFORMAT_JPEG;

    // --------------------------------------------------
    // PSRAM check
    // --------------------------------------------------
    if (psramFound()) {
        Serial.println("PSRAM found");

        // VGA = 640 x 480
        config.frame_size = FRAMESIZE_VGA;

        // Väiksem number = parem JPEG kvaliteet.
        // 10-15 on tavaliselt hea vahemik.
        config.jpeg_quality = 12;

        // Kaks frame bufferit muudavad streami sujuvamaks
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        Serial.println("WARNING: PSRAM not found");

        // Ilma PSRAM-ita kasutame väiksemat pilti
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 15;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }

    // --------------------------------------------------
    // Start camera
    // --------------------------------------------------
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("ERROR: Camera init failed: 0x%x\n", err);
        return false;
    }
    Serial.println("Camera initialized successfully");

    // --------------------------------------------------
    // Detect sensor
    // --------------------------------------------------
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        Serial.println("ERROR: Could not get camera sensor");
        return false;
    }
    Serial.printf("Camera sensor PID: 0x%04X\n", sensor->id.PID);

    // --------------------------------------------------
    // OV3660-specific settings
    // --------------------------------------------------
    if (sensor->id.PID == OV3660_PID) {
        Serial.println("OV3660 detected");

        // Espressifi näites korrigeeritakse OV3660 pilti nii.
        sensor->set_vflip(sensor, 1);
        sensor->set_brightness(sensor, 1);
        sensor->set_saturation(sensor, -2);
        Serial.println("OV3660 image corrections applied");
    } else {
        Serial.println("Camera is not detected as OV3660");
    }
    Serial.println("=======================");
    return true;
}

// ==================================================
// WiFi connection
// ==================================================

bool connectWiFi() {
    Serial.println();
    Serial.println("===== WIFI =====");
    Serial.printf("Connecting to: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);

    // ESP camera stream võib WiFi power-savinguga hakata jõnksutama.
    WiFi.setSleep(false);

    // Toetame nii passwordiga kui open WiFi võrku.
    if (strlen(WIFI_PASSWORD) == 0) {
        Serial.println("Open WiFi network");
        WiFi.begin(WIFI_SSID);
    } else {
        Serial.println("Password-protected WiFi");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    unsigned long startTime = millis();
    const unsigned long timeout = 20000;
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeout) {
        Serial.print(".");
        delay(500);
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR: WiFi connection failed");
        Serial.println("================");
        return false;
    }
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    Serial.println("================");
    return true;
}

// ==================================================
// Root page
// ==================================================

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

// ==================================================
// MJPEG stream
// ==================================================

void handleStream() {
    WiFiClient client = server.client();
    Serial.println();
    Serial.println("Stream client connected");

    // HTTP response header
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
    client.println("Cache-Control: no-cache");
    client.println();
    while (client.connected()) {
        // Küsi kaameralt uus frame
        camera_fb_t* frame = esp_camera_fb_get();
        if (!frame) {
            Serial.println("ERROR: Camera frame capture failed");
            delay(10);
            continue;
        }

        // MJPEG boundary
        client.println("--frame");
        client.println("Content-Type: image/jpeg");
        client.printf("Content-Length: %u\r\n\r\n", frame->len);

        // JPEG bytes brauserisse
        client.write(frame->buf, frame->len);
        client.println();

        // Väga oluline:
        // frame buffer tuleb kaamerale tagasi anda
        esp_camera_fb_return(frame);

        // Anna ESP32-le natuke hingamisruumi
        delay(10);
    }
    Serial.println("Stream client disconnected");
}

// ==================================================
// Start web server
// ==================================================

void startWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/stream", HTTP_GET, handleStream);
    server.begin();
    Serial.println();
    Serial.println("===== CAMERA SERVER =====");
    Serial.print("Camera page: http://");
    Serial.println(WiFi.localIP());
    Serial.print("Direct stream: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/stream");
    Serial.println("=========================");
}

// ==================================================
// Arduino setup
// ==================================================

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println();
    Serial.println("=============================");
    Serial.println("XIAO ESP32S3 TOOL CAMERA");
    Serial.println("=============================");

    // --------------------------------------------------
    // Basic hardware info
    // --------------------------------------------------
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());

    // --------------------------------------------------
    // Camera
    // --------------------------------------------------
    if (!initCamera()) {
        Serial.println();
        Serial.println("Camera startup failed.");
        Serial.println("Check Sense board and camera ribbon connection.");
        return;
    }

    // --------------------------------------------------
    // WiFi
    // --------------------------------------------------
    if (!connectWiFi()) {
        Serial.println();
        Serial.println("WiFi startup failed.");
        return;
    }

    // --------------------------------------------------
    // HTTP server
    // --------------------------------------------------
    startWebServer();
    Serial.println();
    Serial.println("SYSTEM READY");
}

// ==================================================
// Arduino loop
// ==================================================

void loop() {
    server.handleClient();
    delay(2);
}
