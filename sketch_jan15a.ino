#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h> 

// Definisikan pin untuk pembaca RFID RC522
#define RST_PIN 22
#define SS_PIN 21

MFRC522 mfrc522(SS_PIN, RST_PIN);  // Membuat objek MFRC522

// WiFi Settings
const char* ssid = "why";         // Ganti dengan SSID Wi-Fi kamu
const char* password = "wahbagus";   // Ganti dengan password Wi-Fi kamu

// URL endpoint Laravel untuk slot parkir
const char* parkingServerName = "http://192.168.0.105:8000/api/slot_parking";

// Daftar pin sensor IR dan ID slot parkir yang sesuai
const int irPins[] = {13, 34};  // Ganti dengan pin sensor IR yang kamu gunakan
const String slotIds[] = {"A1", "B1"};  // ID slot parkir yang sesuai dengan pin

// Web server pada ESP32
WebServer server(80);

// Servo motor
Servo servo;
const int servoPin = 17; // Ganti dengan pin yang sesuai

// LCD I2C
LiquidCrystal_I2C lcd(0x27, 16, 2); // Alamat I2C (0x27 biasanya default untuk LCD 16x2)

// Mode pengendalian
bool isRegistrationMode = false; // Default adalah mode verifikasi

void setup() {
  Serial.begin(115200);

  // Inisialisasi pembaca RFID
  SPI.begin();
  mfrc522.PCD_Init();
  Wire.begin(25, 26);
  // Inisialisasi servo
  servo.attach(servoPin);
  servo.write(0); // Posisi awal plang tertutup

  // Setup pin sensor IR
  for (int i = 0; i < sizeof(irPins) / sizeof(irPins[0]); i++) {
    pinMode(irPins[i], INPUT);
  }

  // Inisialisasi LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Starting...");
  delay(2000);

  // Menyambungkan ke WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    lcd.setCursor(0, 1);
    lcd.print("Connecting WiFi...");
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP().toString());
  delay(3000);
  lcd.clear();

  // Endpoint untuk mengubah mode
  server.on("/set_mode", HTTP_POST, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*"); // Mengizinkan semua origin
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    if (server.hasArg("mode")) {
      String mode = server.arg("mode");
      if (mode == "registration") {
        isRegistrationMode = true;
        Serial.println("Mode changed to Registration");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Mode: Registration");
        server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Mode changed to registration\"}");
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid mode\"}");
      }
    } else {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Mode parameter is missing\"}");
    }
  });

  // Endpoint untuk mendapatkan ID kartu dalam mode registrasi
  server.on("/get_card_id", HTTP_GET, []() {
    if (isRegistrationMode) {
      String cardID = scanCard();
      server.sendHeader("Access-Control-Allow-Origin", "*"); // Mengizinkan semua origin
      server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      if (cardID != "") {
        server.send(200, "application/json", "{\"card_id\":\"" + cardID + "\"}");
        isRegistrationMode = false; // Kembali ke mode verifikasi setelah registrasi selesai
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Reg Complete");
        delay(2000);
        lcd.clear();
        lcd.print("Mode: Verify");
        Serial.println("Back to verification mode");
      } else {
        server.send(408, "application/json", "{\"status\":\"error\",\"message\":\"No card detected\"}");
      }
    } else {
      server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Not in registration mode\"}");
    }
  });

  server.begin();
  Serial.println("HTTP server started");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();

  // Mode Verifikasi Otomatis
  if (!isRegistrationMode) {
    lcd.setCursor(0, 0);
    lcd.print("Scan your card...");
    lcd.setCursor(0, 1);
    lcd.print("");
    sendParkingData();
    String cardID = scanCard();
    if (cardID != "") {
      Serial.println("Verifying card: " + cardID);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Verifying...");
      if (verifyCard(cardID)) {
        Serial.println("Access granted");
        lcd.setCursor(0, 0);
        lcd.print("Access Granted");
        lcd.setCursor(0, 1);
        lcd.print("Welcome!");
        openGate();
      } else {
        Serial.println("Access denied");
        lcd.setCursor(0, 0);
        lcd.print("Access Denied");
        lcd.setCursor(0, 1);
        lcd.print("Try Again!");
        delay(3000);
        lcd.clear();
      }
    }
  }

  // Kirim data slot parkir ke server

  delay(3000);  // Delay untuk menghindari beban server terlalu berat
}

// Fungsi untuk membaca kartu RFID
String scanCard() {
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String cardID = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      cardID += String(mfrc522.uid.uidByte[i], HEX);
    }
    cardID.toUpperCase();  // Mengubah ID menjadi huruf besar
    mfrc522.PICC_HaltA();  // Hentikan komunikasi dengan kartu
    return cardID;
  }
  return "";  // Tidak ada kartu yang terdeteksi
}

// Fungsi untuk memverifikasi kartu ke server
bool verifyCard(String cardID) {
  WiFiClient client;
  HTTPClient http;

  String url = "http://192.168.0.105:8000/api/verify_card/" + cardID; 
  http.begin(client, url);
  int httpResponseCode = http.GET();

  if (httpResponseCode == 200) {
    String response = http.getString();
    Serial.println("Verification response: " + response);
    http.end();
    return response.indexOf("\"status\":\"valid\"") > 0; // Cek apakah kartu valid
  } else {
    Serial.println("Error during verification");
    http.end();
    return false;
  }
}

// Fungsi untuk membuka plang
void openGate() {
  servo.write(90); // Putar servo ke 90 derajat
  delay(5000);     // Tunggu 5 detik
  servo.write(0);  // Tutup plang kembali
}

// Fungsi untuk mengirimkan data slot parkir
void sendParkingData() {
  for (int i = 0; i < sizeof(irPins) / sizeof(irPins[0]); i++) {
    int sensorStatus = digitalRead(irPins[i]);
    int slotStatus = (sensorStatus == HIGH) ? 0 : 1; // Jika IR tidak terdeteksi (slot kosong), status 0

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(parkingServerName);
      http.addHeader("Content-Type", "application/json");

      String postData = "{\"slot_id\": \"" + slotIds[i] + "\", \"status\": " + String(slotStatus) + "}";
      int httpResponseCode = http.POST(postData);

      if (httpResponseCode > 0) {
        Serial.println("Slot " + slotIds[i] + " status sent successfully");
      } else {
        Serial.println("Failed to send status for slot " + slotIds[i]);
      }

      http.end();
    }
  }
  delay(10000);
}
