#include <WiFiS3.h>
#include <ArduinoJson.h>
#include <WiFiSSLClient.h>
#include <DHT.h>
#include <math.h>

// Wi-Fi設定
const char* ssid = "DK_LAN_Wi-Fi6_2.4Ghz";
const char* password = "dai75684";

// SORACOM API設定（パブリッシュ用）
const char* publishEndpoint = "g.api.soracom.io";
const int port = 443;
const char* deviceID = "F4:12:FA:64:B3:9C";

// SORACOM 認証用設定
// 認証サーバーのエンドポイント（パブリッシュ用とは異なる）
const char* authEndpoint = "api.soracom.io";
const char* authKeyId = "keyId-gAIKmLBySIFHUUSFhWRwgaiUKZFiHZOR";  // SORACOMのAuthKeyId
const char* authKey   = "secret-A3K01dqcqpbtqnZ9Epds1ESqiaPJBWEXzJi0Q13vme4rU6eZvVGV2oJQV4HI52sy";      // SORACOMのAuthKey

// 認証で取得したAPIキーとトークン（自動更新）
String apiKey = "";
String apiToken = "";
unsigned long lastAuthTime = 0;             // 最後に認証した時刻
const long authInterval = 23UL * 60UL * 60UL * 1000UL; // 23時間（ミリ秒）

// SSLクライアントのインスタンス作成
WiFiSSLClient client;

// センサー通信コマンド設定
uint8_t Com[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0x44, 0x09 };

// DHTセンサー設定
#define DHTPIN 3
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// センサー値格納用変数
float soilTemp = 0, soilHumidity = 0, ph = 0, airTemp = 0, airHumidity = 0, vpd = 0;
int ec = 0;

// タイマー設定
unsigned long previousMillis = 0;
const long interval = 10000; // 10秒間隔

// プロトタイプ宣言
bool getSoracomToken();
void reconnectWiFi();
bool readSensorData();
void calculateVPD();
void sendToSORACOM();
uint8_t readN(uint8_t *buf, size_t len);
unsigned int CRC16_2(unsigned char *buf, int len);

void setup() {
  Serial.begin(9600);
  dht.begin();

  // Wi-Fi接続
  Serial.println("WiFiに接続中...");
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 10) {
    delay(1000);
    Serial.print(".");
    retries++;
  }
  if (retries >= 10) {
    Serial.println("\nWiFi接続失敗。リセットしてください。");
    while (1);
  }
  Serial.println("\nWiFi接続成功!");

  // SORACOM 認証（初回認証）
  if (!getSoracomToken()) {
    Serial.println("SORACOM認証に失敗しました。リセットしてください。");
    while (1);
  }
  lastAuthTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // 23時間毎にSORACOMトークン更新
  if (currentMillis - lastAuthTime >= authInterval) {
    if (getSoracomToken()) {
      Serial.println("SORACOMトークン更新成功");
    } else {
      Serial.println("SORACOMトークン更新失敗");
    }
    lastAuthTime = currentMillis;
  }

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    // Wi-Fi接続が切れている場合は再接続
    if (WiFi.status() != WL_CONNECTED) {
      reconnectWiFi();
    }

    // センサーデータの取得、計算、送信
    if (readSensorData()) {
      calculateVPD();
      sendToSORACOM();
    } else {
      Serial.println("センサーデータの読み取りに失敗しました。");
    }
  }
}

// Wi-Fi再接続処理
void reconnectWiFi() {
  Serial.println("WiFi接続が切れました。再接続中...");
  WiFi.disconnect();
  int retries = 0;
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED && retries < 10) {
    delay(1000);
    Serial.print(".");
    retries++;
  }
  if (retries >= 10) {
    Serial.println("\n再接続失敗。リセットしてください。");
    while (1);
  }
  Serial.println("\nWiFi再接続成功!");
}

// SORACOM 認証処理
bool getSoracomToken() {
  client.setTimeout(5000);
  if (!client.connect(authEndpoint, port)) {
    Serial.println("SORACOM認証サーバー接続失敗");
    return false;
  }

  // 認証リクエスト用JSON作成
  StaticJsonDocument<256> doc;
  doc["authKeyId"] = authKeyId;
  doc["authKey"]   = authKey;
  String jsonData;
  serializeJson(doc, jsonData);

  // HTTPリクエスト送信
  client.print("POST /v1/auth HTTP/1.1\r\n");
  client.print("Host: "); client.print(authEndpoint); client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: "); client.print(jsonData.length()); client.print("\r\n\r\n");
  client.print(jsonData);

  // レスポンスの受信
  String response = "";
  unsigned long timeout = millis();
  while (client.connected() || client.available()) {
    if (client.available()) {
      response += client.readString();
    }
    if (millis() - timeout > 5000) break;
  }
  client.stop();

  // JSONレスポンス解析
  StaticJsonDocument<512> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, response);
  if (error) {
    Serial.print("JSON解析エラー: ");
    Serial.println(error.f_str());
    return false;
  }

  // 取得結果の確認
  if (responseDoc.containsKey("apiKey") && responseDoc.containsKey("token")) {
    apiKey = responseDoc["apiKey"].as<String>();
    apiToken = responseDoc["token"].as<String>();
    Serial.println("新しいSORACOMトークン取得成功");
    return true;
  } else {
    Serial.println("SORACOMトークン取得失敗");
    return false;
  }
}

// センサーデータの取得処理
bool readSensorData() {
  uint8_t Data[13] = { 0 };
  uint8_t ch = 0;
  bool flag = true;
  int retries = 0;

  while (flag && retries < 5) {  // 最大5回試行
    retries++;
    delay(100);
    Serial.write(Com, 8);  // コマンド送信
    delay(50);

    // センサーからのデータ受信処理
    if (readN(&ch, 1) == 1 && ch == 0x01) {
      Data[0] = ch;
      if (readN(&ch, 1) == 1 && ch == 0x03) {
        Data[1] = ch;
        if (readN(&ch, 1) == 1 && ch == 0x08) {
          Data[2] = ch;
          if (readN(&Data[3], 10) == 10) { // 10バイト受信
            if (CRC16_2(Data, 11) == (Data[11] * 256 + Data[12])) {
              // センサーデータ解析
              soilHumidity = (Data[3] * 256 + Data[4]) / 10.0;
              soilTemp     = (Data[5] * 256 + Data[6]) / 10.0;
              ec           = Data[7] * 256 + Data[8];
              ph           = (Data[9] * 256 + Data[10]) / 10.0;
              flag = false;  // 成功
            }
          }
        }
      }
    }
    Serial.flush();
  }

  if (retries >= 5) {
    Serial.println("センサー応答エラー");
    return false;
  }

  // DHTセンサーから空気温度・湿度を取得
  float newAirTemp = dht.readTemperature();
  float newAirHumidity = dht.readHumidity();

  if (!isnan(newAirTemp)) {
    airTemp = newAirTemp;
  } else {
    Serial.println("DHT温度読み取り失敗");
  }

  if (!isnan(newAirHumidity)) {
    airHumidity = newAirHumidity;
  } else {
    Serial.println("DHT湿度読み取り失敗");
  }

  // センサーデータの値チェック
  if (soilHumidity < 0 || soilHumidity > 100 || ph < 0 || ph > 14) {
    Serial.println("センサーデータが異常値");
    return false;
  }

  return true;
}

// VPD（蒸気圧差）計算処理
void calculateVPD() {
  if (isnan(airTemp) || isnan(airHumidity) || airHumidity < 0 || airHumidity > 100) {
    Serial.println("VPD計算エラー: 無効なデータ");
    vpd = NAN;
    return;
  }
  float es = 0.6108 * exp((17.27 * airTemp) / (airTemp + 237.3));  // 飽和水蒸気圧
  float ea = (airHumidity / 100.0) * es;  // 実際の水蒸気圧
  vpd = es - ea;
  if (vpd < 0) vpd = 0;
}

// SORACOMへデータ送信
void sendToSORACOM() {
  client.setTimeout(5000);
  if (client.connect(publishEndpoint, port)) {  // 接続
    StaticJsonDocument<512> doc;
    // JSONデータ作成
    doc["soilTemperature"] = soilTemp;
    doc["soilHumidity"]    = soilHumidity;
    doc["ec"]              = ec;
    doc["ph"]              = ph;
    doc["airTemperature"]  = airTemp;
    doc["airHumidity"]     = airHumidity;
    doc["vpd"]             = vpd;
    String jsonData;
    serializeJson(doc, jsonData);

    // HTTPリクエスト作成
    client.print("POST /v1/devices/");
    client.print(deviceID);
    client.print("/publish HTTP/1.1\r\nHost: ");
    client.print(publishEndpoint);
    client.print("\r\nContent-Type: application/json\r\nContent-Length: ");
    client.print(jsonData.length());
    client.print("\r\nX-Soracom-API-Key: ");
    client.print(apiKey);
    client.print("\r\nX-Soracom-Token: ");
    client.print(apiToken);
    client.print("\r\n\r\n");
    client.print(jsonData);

    // レスポンス受信（ヘッダー部分のみ読み飛ばす）
    while (client.connected() || client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r") break;
    }
    client.stop();
  } else {
    Serial.println("SORACOM接続失敗");
  }
}

// 指定したバイト数分、シリアルから読み出す関数
uint8_t readN(uint8_t *buf, size_t len) {
  size_t offset = 0, left = len;
  int timeout = 500;
  long start = millis();

  while (left) {
    if (Serial.available()) {
      buf[offset] = Serial.read();
      offset++;
      left--;
    }
    if (millis() - start > timeout) break;
  }
  return offset;
}

// CRC16計算（バイト順反転あり）
unsigned int CRC16_2(unsigned char *buf, int len) {
  unsigned int crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (unsigned int)buf[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return (crc << 8) | (crc >> 8); // バイト順を反転
}


