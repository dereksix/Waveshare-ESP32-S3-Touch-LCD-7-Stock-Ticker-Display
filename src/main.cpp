// Stock Ticker for ESP32-S3 with 7" Touch Display
// Displays real-time stock quotes using TwelveData API
// Hardware: Waveshare ESP32-S3-Touch-LCD-7 (800x480)
// 
// IMPORTANT: Copy include/config.example.h to include/config.h and add your API key
// LVGL port runs its own task, so we must use lvgl_port_lock/unlock

#define FIRMWARE_VERSION "1.9.25"
#define GITHUB_REPO "dereksix/Waveshare-ESP32-S3-Touch-LCD-7-Stock-Ticker-Display"

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include "config.h"

using namespace esp_panel::board;

Preferences prefs;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000, 60000);

// OTA Web Server
WebServer otaServer(80);
bool otaInProgress = false;
String otaStatus = "";

// GitHub OTA state
bool pendingGitHubOTA = false;
lv_obj_t *otaProgressPopup = nullptr;
lv_obj_t *otaProgressLabel = nullptr;
lv_obj_t *otaProgressBar = nullptr;

// UI elements
lv_obj_t *priceLabel = nullptr;
lv_obj_t *changeLabel = nullptr;
lv_obj_t *dollarChangeLabel = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *settingsPopup = nullptr;
lv_obj_t *symbolLabel = nullptr;
lv_obj_t *rangeBar = nullptr;
lv_obj_t *rangeLowLabel = nullptr;
lv_obj_t *rangeHighLabel = nullptr;
lv_obj_t *ohlLabel = nullptr;
lv_obj_t *volumeLabel = nullptr;
lv_obj_t *wifiIcon = nullptr;
lv_obj_t *trendArrow = nullptr;
lv_obj_t *trendPanel = nullptr;
lv_obj_t *marketStatusLabel = nullptr;
lv_obj_t *companyNameLabel = nullptr;
lv_obj_t *fiftyTwoWeekBar = nullptr;
lv_obj_t *fiftyTwoWeekLowLabel = nullptr;
lv_obj_t *fiftyTwoWeekHighLabel = nullptr;

// State flags
String currentSymbol = "MSFT";
String lastPrice = "N/A";
String lastChange = "0.0";
String lastDollarChange = "0.0";
String previousClose = "0.0";
bool isMarketOpen = false;  // Track market state for smart refresh
int pendingTickerIndex = -1;
bool pendingClosePopup = false;
bool pendingFetch = false;
bool pendingOpenSettings = false;

// Custom symbol input state
lv_obj_t *customSymbolTA = nullptr;
lv_obj_t *customSymbolKeyboard = nullptr;
bool pendingCustomSymbol = false;
String pendingCustomSymbolStr = "";

// Stock rotation state
bool rotationEnabled = false;
String rotationList = "";
String rotationSymbols[20];
int rotationCount = 0;
int rotationIndex = 0;
uint32_t lastRotationTime = 0;
int rotationIntervalMins = 5;  // Default 5 minutes
lv_obj_t *rotationTA = nullptr;
lv_obj_t *rotationSwitch = nullptr;
lv_obj_t *rotationKeyboard = nullptr;
lv_obj_t *rotationPopup = nullptr;
lv_obj_t *rotationIntervalDropdown = nullptr;
bool pendingRotation = false;

// Clock display
lv_obj_t *clockLabel = nullptr;

// Swipe detection state
static lv_point_t swipeStart;
static bool swipeTracking = false;

// Cached data for error recovery and market-closed optimization
struct CachedStockData {
  bool valid;
  String symbol;
  String priceStr;
  String changeStr;
  String dollarChangeStr;
  String ohlStr;
  String volumeStr;
  String companyName;
  float low, high, fiftyTwoLow, fiftyTwoHigh;
  int dayRangePos, fiftyTwoPos;
  bool marketOpen;
  uint32_t fetchTime;  // millis() when data was fetched
} cachedData = {false};

// Multi-symbol cache for rotation (up to 20 symbols)
CachedStockData symbolCache[20];
int symbolCacheCount = 0;

// Find cached data for a symbol
CachedStockData* findCachedSymbol(const String& symbol) {
  for (int i = 0; i < symbolCacheCount; i++) {
    if (symbolCache[i].valid && symbolCache[i].symbol == symbol) {
      return &symbolCache[i];
    }
  }
  return nullptr;
}

// Add or update symbol in cache
void cacheSymbolData(const CachedStockData& data) {
  // Check if symbol already exists in cache
  for (int i = 0; i < symbolCacheCount; i++) {
    if (symbolCache[i].symbol == data.symbol) {
      symbolCache[i] = data;
      return;
    }
  }
  // Add new entry if space available
  if (symbolCacheCount < 20) {
    symbolCache[symbolCacheCount++] = data;
  }
}

// Last time we checked if market reopened (when closed)
uint32_t lastMarketCheck = 0;
const uint32_t MARKET_CLOSED_CHECK_INTERVAL = 3600000;  // 1 hour (default)
const uint32_t MARKET_TRANSITION_CHECK_INTERVAL = 300000;  // 5 minutes (near open/close)

// Check if we're near market open (9:00-10:00 AM ET) or close (3:30-4:30 PM ET)
bool isNearMarketTransition() {
  int hours = timeClient.getHours();
  int mins = timeClient.getMinutes();
  int totalMins = hours * 60 + mins;
  
  // Near market open: 9:00 AM - 10:00 AM ET (540-600 minutes)
  if (totalMins >= 540 && totalMins <= 600) return true;
  
  // Near market close: 3:30 PM - 4:30 PM ET (930-990 minutes)
  if (totalMins >= 930 && totalMins <= 990) return true;
  
  return false;
}

// Prefetched stock data for smooth transitions
struct PrefetchedData {
  bool valid;
  String symbol;
  float closePrice;
  float prevClose;
  float pctChange;
  float openPrice;
  float highPrice;
  float lowPrice;
  float volume;
  float fiftyTwoLow;
  float fiftyTwoHigh;
  String companyName;
  bool marketOpen;
};
PrefetchedData prefetchedStock = {false};

// WiFi setup state
lv_obj_t *wifiPopup = nullptr;
lv_obj_t *wifiList = nullptr;
lv_obj_t *wifiPasswordTA = nullptr;
lv_obj_t *wifiKeyboard = nullptr;
lv_obj_t *wifiStatusLbl = nullptr;
String selectedSSID = "";
char scannedNetworks[10][33];
int numScannedNetworks = 0;
bool wifiScanInProgress = false;
bool pendingOpenWifi = false;
bool pendingCloseWifi = false;
bool pendingWifiConnect = false;
bool pendingShowKeyboard = false;
int pendingNetworkIndex = -1;

// API Key - stored in Preferences, falls back to config.h
String apiKey = "";  // Loaded from Preferences on startup

// Tickers
const char* tickers[] = {"MSFT", "AAPL", "GOOGL", "AMZN", "NVDA", "TSLA", "META", "SPY", "QQQ"};
const int numTickers = 9;

// Parse comma-separated rotation list
void parseRotationList() {
  rotationCount = 0;
  if (rotationList.length() == 0) return;
  
  String temp = rotationList;
  temp.trim();
  temp.toUpperCase();
  
  int start = 0;
  for (int i = 0; i <= temp.length() && rotationCount < 20; i++) {
    if (i == temp.length() || temp[i] == ',') {
      String symbol = temp.substring(start, i);
      symbol.trim();
      if (symbol.length() > 0 && symbol.length() <= 10) {
        rotationSymbols[rotationCount++] = symbol;
      }
      start = i + 1;
    }
  }
  rotationIndex = 0;
}

// Prefetch stock data for a symbol (for smooth rotation)
// Uses cached data when market is closed to save API calls
bool prefetchStockData(const String& symbol) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  // Check if market is closed and we have cached data for this symbol
  if (!isMarketOpen) {
    CachedStockData* cached = findCachedSymbol(symbol);
    if (cached != nullptr && cached->valid) {
      // Use cached data - no API call needed!
      Serial.printf("Market closed - using cached data for %s\n", symbol.c_str());
      
      // Convert cached display data back to prefetchedStock
      // We need to parse the cached strings back to values
      prefetchedStock.symbol = cached->symbol;
      prefetchedStock.companyName = cached->companyName;
      prefetchedStock.lowPrice = cached->low;
      prefetchedStock.highPrice = cached->high;
      prefetchedStock.fiftyTwoLow = cached->fiftyTwoLow;
      prefetchedStock.fiftyTwoHigh = cached->fiftyTwoHigh;
      prefetchedStock.marketOpen = cached->marketOpen;
      
      // Parse price from cached string (e.g., "$485.92")
      String priceStr = cached->priceStr;
      priceStr.replace("$", "");
      priceStr.replace(",", "");
      prefetchedStock.closePrice = priceStr.toFloat();
      
      // Parse percent change from cached string (e.g., "+0.40%" or "-1.23%")
      String pctStr = cached->changeStr;
      pctStr.replace("%", "");
      pctStr.replace("+", "");
      prefetchedStock.pctChange = pctStr.toFloat();
      
      // Parse dollar change (e.g., "+$1.94" or "-$2.50")
      String dollarStr = cached->dollarChangeStr;
      dollarStr.replace("$", "");
      dollarStr.replace("+", "");
      float dollarChange = dollarStr.toFloat();
      prefetchedStock.prevClose = prefetchedStock.closePrice - dollarChange;
      
      // Parse volume from cached string (e.g., "Vol: 70.82M")
      String volStr = cached->volumeStr;
      volStr.replace("Vol: ", "");
      float volMult = 1.0;
      if (volStr.endsWith("B")) { volMult = 1000000000.0; volStr.replace("B", ""); }
      else if (volStr.endsWith("M")) { volMult = 1000000.0; volStr.replace("M", ""); }
      else if (volStr.endsWith("K")) { volMult = 1000.0; volStr.replace("K", ""); }
      prefetchedStock.volume = volStr.toFloat() * volMult;
      
      // Parse open price from OHL string (e.g., "O: 487.36  H: 487.85  L: 482.49")
      String ohlStr = cached->ohlStr;
      int oIdx = ohlStr.indexOf("O: ");
      int hIdx = ohlStr.indexOf("H: ");
      if (oIdx >= 0 && hIdx > oIdx) {
        prefetchedStock.openPrice = ohlStr.substring(oIdx + 3, hIdx).toFloat();
      }
      
      prefetchedStock.valid = true;
      return true;
    }
    // No cached data - need to fetch once even when closed
    Serial.printf("Market closed but no cache for %s - fetching once\n", symbol.c_str());
  }
  
  HTTPClient http;
  String url = "https://api.twelvedata.com/quote?symbol=" + symbol + "&apikey=" + apiKey;
  
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    
    prefetchedStock.symbol = symbol;
    prefetchedStock.closePrice = 0.0;
    prefetchedStock.prevClose = 0.0;
    prefetchedStock.pctChange = 0.0;
    prefetchedStock.openPrice = 0.0;
    prefetchedStock.highPrice = 0.0;
    prefetchedStock.lowPrice = 0.0;
    prefetchedStock.volume = 0.0;
    prefetchedStock.fiftyTwoLow = 0.0;
    prefetchedStock.fiftyTwoHigh = 0.0;
    prefetchedStock.companyName = "";
    prefetchedStock.marketOpen = false;
    
    if (doc["close"].is<const char*>()) prefetchedStock.closePrice = atof(doc["close"].as<const char*>());
    if (doc["previous_close"].is<const char*>()) prefetchedStock.prevClose = atof(doc["previous_close"].as<const char*>());
    if (doc["percent_change"].is<const char*>()) prefetchedStock.pctChange = atof(doc["percent_change"].as<const char*>());
    if (doc["open"].is<const char*>()) prefetchedStock.openPrice = atof(doc["open"].as<const char*>());
    if (doc["high"].is<const char*>()) prefetchedStock.highPrice = atof(doc["high"].as<const char*>());
    if (doc["low"].is<const char*>()) prefetchedStock.lowPrice = atof(doc["low"].as<const char*>());
    if (doc["volume"].is<const char*>()) prefetchedStock.volume = atof(doc["volume"].as<const char*>());
    if (doc["name"].is<const char*>()) prefetchedStock.companyName = doc["name"].as<const char*>();
    if (doc["is_market_open"].is<bool>()) prefetchedStock.marketOpen = doc["is_market_open"].as<bool>();
    if (doc["fifty_two_week"]["low"].is<const char*>()) 
      prefetchedStock.fiftyTwoLow = atof(doc["fifty_two_week"]["low"].as<const char*>());
    if (doc["fifty_two_week"]["high"].is<const char*>()) 
      prefetchedStock.fiftyTwoHigh = atof(doc["fifty_two_week"]["high"].as<const char*>());
    
    // Update global market status
    isMarketOpen = prefetchedStock.marketOpen;
    
    prefetchedStock.valid = true;
    http.end();
    return true;
  }
  
  http.end();
  prefetchedStock.valid = false;
  return false;
}

// Apply prefetched data to UI (call with LVGL lock held)
void applyPrefetchedData() {
  if (!prefetchedStock.valid) return;
  
  currentSymbol = prefetchedStock.symbol;
  float closePrice = prefetchedStock.closePrice;
  float pctChange = prefetchedStock.pctChange;
  float dollarChange = closePrice - prefetchedStock.prevClose;
  
  // Format strings
  char priceBuf[16], pctBuf[16], dollarBuf[16];
  snprintf(priceBuf, sizeof(priceBuf), "$%.2f", closePrice);
  snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", pctChange);
  snprintf(dollarBuf, sizeof(dollarBuf), "%+.2f", dollarChange);
  
  char ohlBuf[48];
  snprintf(ohlBuf, sizeof(ohlBuf), "O: %.2f   H: %.2f   L: %.2f", 
           prefetchedStock.openPrice, prefetchedStock.highPrice, prefetchedStock.lowPrice);
  
  char volBuf[24];
  float volume = prefetchedStock.volume;
  if (volume >= 1e9) snprintf(volBuf, sizeof(volBuf), "Vol: %.2fB", volume / 1e9);
  else if (volume >= 1e6) snprintf(volBuf, sizeof(volBuf), "Vol: %.2fM", volume / 1e6);
  else if (volume >= 1e3) snprintf(volBuf, sizeof(volBuf), "Vol: %.1fK", volume / 1e3);
  else snprintf(volBuf, sizeof(volBuf), "Vol: %.0f", volume);
  
  // Range calculations
  int rangePos = 50;
  if (prefetchedStock.highPrice > prefetchedStock.lowPrice) {
    rangePos = (int)(((closePrice - prefetchedStock.lowPrice) / (prefetchedStock.highPrice - prefetchedStock.lowPrice)) * 100);
    if (rangePos < 0) rangePos = 0;
    if (rangePos > 100) rangePos = 100;
  }
  
  int fiftyTwoPos = 50;
  if (prefetchedStock.fiftyTwoHigh > prefetchedStock.fiftyTwoLow) {
    fiftyTwoPos = (int)(((closePrice - prefetchedStock.fiftyTwoLow) / (prefetchedStock.fiftyTwoHigh - prefetchedStock.fiftyTwoLow)) * 100);
    if (fiftyTwoPos < 0) fiftyTwoPos = 0;
    if (fiftyTwoPos > 100) fiftyTwoPos = 100;
  }
  
  char lowBuf[12], highBuf[12];
  snprintf(lowBuf, sizeof(lowBuf), "%.2f", prefetchedStock.lowPrice);
  snprintf(highBuf, sizeof(highBuf), "%.2f", prefetchedStock.highPrice);
  
  char fiftyTwoLowBuf[12], fiftyTwoHighBuf[12];
  snprintf(fiftyTwoLowBuf, sizeof(fiftyTwoLowBuf), "%.2f", prefetchedStock.fiftyTwoLow);
  snprintf(fiftyTwoHighBuf, sizeof(fiftyTwoHighBuf), "%.2f", prefetchedStock.fiftyTwoHigh);
  
  // Update company name and symbol separately
  if (prefetchedStock.companyName.length() > 0) {
    lv_label_set_text(companyNameLabel, prefetchedStock.companyName.c_str());
  } else {
    lv_label_set_text(companyNameLabel, "");
  }
  char symbolBuf[16];
  snprintf(symbolBuf, sizeof(symbolBuf), "$%s", currentSymbol.c_str());
  lv_label_set_text(symbolLabel, symbolBuf);
  
  lv_label_set_text(priceLabel, priceBuf);
  lv_label_set_text(changeLabel, pctBuf);
  lv_label_set_text(dollarChangeLabel, dollarBuf);
  lv_label_set_text(ohlLabel, ohlBuf);
  lv_label_set_text(volumeLabel, volBuf);
  lv_label_set_text(rangeLowLabel, lowBuf);
  lv_label_set_text(rangeHighLabel, highBuf);
  lv_bar_set_value(rangeBar, rangePos, LV_ANIM_OFF);
  
  lv_color_t changeColor = pctChange >= 0 ? lv_color_hex(0x00E676) : lv_color_hex(0xFF5252);
  
  lv_label_set_text(trendArrow, pctChange >= 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(trendArrow, changeColor, 0);
  lv_obj_set_style_border_color(trendPanel, changeColor, 0);
  
  lv_label_set_text(fiftyTwoWeekLowLabel, fiftyTwoLowBuf);
  lv_label_set_text(fiftyTwoWeekHighLabel, fiftyTwoHighBuf);
  lv_bar_set_value(fiftyTwoWeekBar, fiftyTwoPos, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(fiftyTwoWeekBar, changeColor, LV_PART_INDICATOR);
  
  lv_label_set_text(marketStatusLabel, prefetchedStock.marketOpen ? "Market Open" : "Market Closed");
  lv_obj_set_style_text_color(marketStatusLabel, 
    prefetchedStock.marketOpen ? lv_color_hex(0x00E676) : lv_color_hex(0xFF9800), 0);
  
  isMarketOpen = prefetchedStock.marketOpen;
  
  lv_obj_set_style_text_color(changeLabel, changeColor, 0);
  lv_obj_set_style_text_color(dollarChangeLabel, changeColor, 0);
  lv_obj_set_style_bg_color(rangeBar, changeColor, LV_PART_INDICATOR);
  
  timeClient.update();
  int hour = timeClient.getHours();
  int minute = timeClient.getMinutes();
  char timeBuf[64];
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(timeBuf, sizeof(timeBuf), "Last Updated: %d:%02d %s  |  $MSFT Money Team", hour12, minute, hour >= 12 ? "PM" : "AM");
  lv_label_set_text(statusLabel, timeBuf);
  
  // Cache this data for rotation when market closed
  CachedStockData newCache;
  newCache.valid = true;
  newCache.symbol = currentSymbol;
  newCache.priceStr = priceBuf;
  newCache.changeStr = pctBuf;
  newCache.dollarChangeStr = dollarBuf;
  newCache.ohlStr = ohlBuf;
  newCache.volumeStr = volBuf;
  newCache.companyName = prefetchedStock.companyName;
  newCache.low = prefetchedStock.lowPrice;
  newCache.high = prefetchedStock.highPrice;
  newCache.fiftyTwoLow = prefetchedStock.fiftyTwoLow;
  newCache.fiftyTwoHigh = prefetchedStock.fiftyTwoHigh;
  newCache.dayRangePos = rangePos;
  newCache.fiftyTwoPos = fiftyTwoPos;
  newCache.marketOpen = prefetchedStock.marketOpen;
  newCache.fetchTime = millis();
  cacheSymbolData(newCache);
  
  prefetchedStock.valid = false;  // Mark as consumed
}

void fetchPrice() {
  if (WiFi.status() != WL_CONNECTED) {
    if (lvgl_port_lock(100)) {
      lv_label_set_text(statusLabel, "No WiFi");
      lv_obj_invalidate(statusLabel);
      lvgl_port_unlock();
    }
    return;
  }
  
  HTTPClient http;
  String url = "https://api.twelvedata.com/quote?symbol=" + currentSymbol + "&apikey=" + apiKey;
  
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    
    float closePrice = 0.0, prevClose = 0.0, pctChange = 0.0;
    float openPrice = 0.0, highPrice = 0.0, lowPrice = 0.0;
    float volume = 0.0;
    float fiftyTwoLow = 0.0, fiftyTwoHigh = 0.0;
    String companyName = "";
    bool apiMarketOpen = false;
    
    if (doc["close"].is<const char*>()) closePrice = atof(doc["close"].as<const char*>());
    if (doc["previous_close"].is<const char*>()) prevClose = atof(doc["previous_close"].as<const char*>());
    if (doc["percent_change"].is<const char*>()) pctChange = atof(doc["percent_change"].as<const char*>());
    if (doc["open"].is<const char*>()) openPrice = atof(doc["open"].as<const char*>());
    if (doc["high"].is<const char*>()) highPrice = atof(doc["high"].as<const char*>());
    if (doc["low"].is<const char*>()) lowPrice = atof(doc["low"].as<const char*>());
    if (doc["volume"].is<const char*>()) volume = atof(doc["volume"].as<const char*>());
    if (doc["name"].is<const char*>()) companyName = doc["name"].as<const char*>();
    if (doc["is_market_open"].is<bool>()) apiMarketOpen = doc["is_market_open"].as<bool>();
    
    // Parse 52-week range
    if (doc["fifty_two_week"]["low"].is<const char*>()) 
      fiftyTwoLow = atof(doc["fifty_two_week"]["low"].as<const char*>());
    if (doc["fifty_two_week"]["high"].is<const char*>()) 
      fiftyTwoHigh = atof(doc["fifty_two_week"]["high"].as<const char*>());
    
    float dollarChange = closePrice - prevClose;
    
    // Format strings
    char priceBuf[16], pctBuf[16], dollarBuf[16];
    snprintf(priceBuf, sizeof(priceBuf), "$%.2f", closePrice);
    snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", pctChange);
    snprintf(dollarBuf, sizeof(dollarBuf), "%+.2f", dollarChange);
    
    // Open/High/Low string
    char ohlBuf[48];
    snprintf(ohlBuf, sizeof(ohlBuf), "O: %.2f   H: %.2f   L: %.2f", openPrice, highPrice, lowPrice);
    
    // Volume (format with K/M/B suffix)
    char volBuf[24];
    if (volume >= 1e9) snprintf(volBuf, sizeof(volBuf), "Vol: %.2fB", volume / 1e9);
    else if (volume >= 1e6) snprintf(volBuf, sizeof(volBuf), "Vol: %.2fM", volume / 1e6);
    else if (volume >= 1e3) snprintf(volBuf, sizeof(volBuf), "Vol: %.1fK", volume / 1e3);
    else snprintf(volBuf, sizeof(volBuf), "Vol: %.0f", volume);
    
    lastPrice = String(priceBuf);
    lastChange = String(pctBuf);
    lastDollarChange = String(dollarBuf);
    
    // Calculate range bar position (0-100)
    int rangePos = 50;
    if (highPrice > lowPrice) {
      rangePos = (int)(((closePrice - lowPrice) / (highPrice - lowPrice)) * 100);
      if (rangePos < 0) rangePos = 0;
      if (rangePos > 100) rangePos = 100;
    }
    
    // Calculate 52-week range position (0-100)
    int fiftyTwoPos = 50;
    if (fiftyTwoHigh > fiftyTwoLow) {
      fiftyTwoPos = (int)(((closePrice - fiftyTwoLow) / (fiftyTwoHigh - fiftyTwoLow)) * 100);
      if (fiftyTwoPos < 0) fiftyTwoPos = 0;
      if (fiftyTwoPos > 100) fiftyTwoPos = 100;
    }
    
    // Get timestamp for "Last Updated" display
    timeClient.update();
    int hour = timeClient.getHours();
    int minute = timeClient.getMinutes();
    
    char timeBuf[64];
    int hour12 = hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(timeBuf, sizeof(timeBuf), "Last Updated: %d:%02d %s  |  $MSFT Money Team", hour12, minute, hour >= 12 ? "PM" : "AM");
    
    // Low/High labels for day range bar
    char lowBuf[12], highBuf2[12];
    snprintf(lowBuf, sizeof(lowBuf), "%.2f", lowPrice);
    snprintf(highBuf2, sizeof(highBuf2), "%.2f", highPrice);
    
    // 52-week range labels
    char fiftyTwoLowBuf[12], fiftyTwoHighBuf[12];
    snprintf(fiftyTwoLowBuf, sizeof(fiftyTwoLowBuf), "%.2f", fiftyTwoLow);
    snprintf(fiftyTwoHighBuf, sizeof(fiftyTwoHighBuf), "%.2f", fiftyTwoHigh);
    
    if (lvgl_port_lock(100)) {
      // Update company name and ticker separately
      if (companyName.length() > 0) {
        lv_label_set_text(companyNameLabel, companyName.c_str());
      } else {
        lv_label_set_text(companyNameLabel, "");
      }
      char symbolBuf[16];
      snprintf(symbolBuf, sizeof(symbolBuf), "$%s", currentSymbol.c_str());
      lv_label_set_text(symbolLabel, symbolBuf);
      
      lv_label_set_text(priceLabel, priceBuf);
      lv_label_set_text(changeLabel, pctBuf);
      lv_label_set_text(dollarChangeLabel, dollarBuf);
      lv_label_set_text(ohlLabel, ohlBuf);
      lv_label_set_text(volumeLabel, volBuf);
      lv_label_set_text(rangeLowLabel, lowBuf);
      lv_label_set_text(rangeHighLabel, highBuf2);
      
      // Update day range bar
      lv_bar_set_value(rangeBar, rangePos, LV_ANIM_ON);
      
      // Update trend panel
      lv_color_t changeColor = pctChange >= 0 ? lv_color_hex(0x00E676) : lv_color_hex(0xFF5252);
      
      // Update big trend arrow
      lv_label_set_text(trendArrow, pctChange >= 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
      lv_obj_set_style_text_color(trendArrow, changeColor, 0);
      
      // Update panel border color
      lv_obj_set_style_border_color(trendPanel, changeColor, 0);
      
      // Update 52-week range bar and labels
      lv_label_set_text(fiftyTwoWeekLowLabel, fiftyTwoLowBuf);
      lv_label_set_text(fiftyTwoWeekHighLabel, fiftyTwoHighBuf);
      lv_bar_set_value(fiftyTwoWeekBar, fiftyTwoPos, LV_ANIM_ON);
      lv_obj_set_style_bg_color(fiftyTwoWeekBar, changeColor, LV_PART_INDICATOR);
      
      // Update market status (now positioned under volume in main area)
      lv_label_set_text(marketStatusLabel, apiMarketOpen ? "Market Open" : "Market Closed");
      lv_obj_set_style_text_color(marketStatusLabel, 
        apiMarketOpen ? lv_color_hex(0x00E676) : lv_color_hex(0xFF9800), 0);
      
      // Store market state globally for smart refresh
      isMarketOpen = apiMarketOpen;
      
      lv_obj_set_style_text_color(changeLabel, changeColor, 0);
      lv_obj_set_style_text_color(dollarChangeLabel, changeColor, 0);
      lv_obj_set_style_bg_color(rangeBar, changeColor, LV_PART_INDICATOR);
      
      // Update WiFi icon color
      if (wifiIcon) {
        lv_obj_set_style_text_color(wifiIcon, lv_color_hex(0x00E676), 0);
      }
      
      lv_label_set_text(statusLabel, timeBuf);
      lv_obj_invalidate(statusLabel);
      lvgl_port_unlock();
    }
    
    // Cache this data for error recovery AND multi-symbol rotation cache
    cachedData.valid = true;
    cachedData.symbol = currentSymbol;
    cachedData.priceStr = priceBuf;
    cachedData.changeStr = pctBuf;
    cachedData.dollarChangeStr = dollarBuf;
    cachedData.ohlStr = ohlBuf;
    cachedData.volumeStr = volBuf;
    cachedData.companyName = companyName;
    cachedData.low = lowPrice;
    cachedData.high = highPrice;
    cachedData.fiftyTwoLow = fiftyTwoLow;
    cachedData.fiftyTwoHigh = fiftyTwoHigh;
    cachedData.dayRangePos = rangePos;
    cachedData.fiftyTwoPos = fiftyTwoPos;
    cachedData.marketOpen = apiMarketOpen;
    cachedData.fetchTime = millis();
    
    // Also add to multi-symbol cache for rotation
    cacheSymbolData(cachedData);
    
    prefs.begin("stock", false);
    prefs.putString("symbol", currentSymbol);
    prefs.putString("price", lastPrice);
    prefs.end();
  } else {
    // API error - show cached data if available
    if (lvgl_port_lock(100)) {
      if (cachedData.valid && cachedData.symbol == currentSymbol) {
        lv_label_set_text(statusLabel, "Cached (API Error)");
      } else {
        lv_label_set_text(statusLabel, "API Error");
      }
      lv_obj_invalidate(statusLabel);
      lvgl_port_unlock();
    }
  }
  http.end();
}

// ============ EVENT CALLBACKS - Only set flags ============

static void ticker_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < numTickers) {
    pendingTickerIndex = idx;
    pendingClosePopup = true;
  }
}

static void close_popup_cb(lv_event_t *e) {
  pendingClosePopup = true;
}

static void open_settings_cb(lv_event_t *e) {
  pendingOpenSettings = true;
}

static void wifi_btn_cb(lv_event_t *e) {
  pendingOpenWifi = true;
  pendingClosePopup = true;
}

static void wifi_network_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < numScannedNetworks) {
    pendingNetworkIndex = idx;
    pendingShowKeyboard = true;
  }
}

static void close_wifi_cb(lv_event_t *e) {
  pendingCloseWifi = true;
}

static void connect_wifi_cb(lv_event_t *e) {
  pendingWifiConnect = true;
}

static void custom_symbol_go_cb(lv_event_t *e) {
  if (customSymbolTA) {
    const char* text = lv_textarea_get_text(customSymbolTA);
    if (text && strlen(text) > 0) {
      pendingCustomSymbolStr = String(text);
      pendingCustomSymbolStr.toUpperCase();
      pendingCustomSymbol = true;
      pendingClosePopup = true;
    }
  }
}

static void custom_symbol_ta_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    if (customSymbolKeyboard) {
      lv_obj_clear_flag(customSymbolKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (rotationKeyboard) {
      lv_obj_add_flag(rotationKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
    if (customSymbolKeyboard) {
      lv_obj_add_flag(customSymbolKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// ============ UI CREATION FUNCTIONS ============

void buildWifiNetworkList() {
  if (!wifiPopup || !wifiList) return;
  
  lv_obj_clean(wifiList);
  
  if (numScannedNetworks == 0) {
    lv_obj_t *noNet = lv_label_create(wifiList);
    lv_label_set_text(noNet, "No networks found");
    lv_obj_set_style_text_color(noNet, lv_color_hex(0xFF6666), 0);
    return;
  }
  
  for (int i = 0; i < numScannedNetworks; i++) {
    lv_obj_t *btn = lv_btn_create(wifiList);
    lv_obj_set_size(btn, 250, 38);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn, 5, 0);
    
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, scannedNetworks[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl, 230);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 5, 0);
    
    lv_obj_add_event_cb(btn, wifi_network_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

void showWifiKeyboard() {
  if (!wifiPopup || wifiKeyboard) return;
  
  // Highlight selected network
  for (int i = 0; i < numScannedNetworks; i++) {
    lv_obj_t *btn = lv_obj_get_child(wifiList, i);
    if (btn) {
      lv_obj_set_style_bg_color(btn, (i == pendingNetworkIndex) ? lv_color_hex(0x00AA00) : lv_color_hex(0x333333), 0);
    }
  }
  
  // Password label
  lv_obj_t *passLbl = lv_label_create(wifiPopup);
  lv_label_set_text(passLbl, "Password:");
  lv_obj_set_style_text_color(passLbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(passLbl, LV_ALIGN_TOP_MID, 100, 45);
  
  // Password text area
  wifiPasswordTA = lv_textarea_create(wifiPopup);
  lv_textarea_set_one_line(wifiPasswordTA, true);
  lv_textarea_set_password_mode(wifiPasswordTA, true);
  lv_textarea_set_max_length(wifiPasswordTA, 64);
  lv_textarea_set_placeholder_text(wifiPasswordTA, "Enter password");
  lv_obj_set_size(wifiPasswordTA, 280, 45);
  lv_obj_align(wifiPasswordTA, LV_ALIGN_TOP_RIGHT, -20, 70);
  lv_obj_set_style_text_font(wifiPasswordTA, &lv_font_montserrat_16, 0);
  
  // Keyboard
  wifiKeyboard = lv_keyboard_create(wifiPopup);
  lv_keyboard_set_textarea(wifiKeyboard, wifiPasswordTA);
  lv_obj_set_size(wifiKeyboard, 440, 200);
  lv_obj_align(wifiKeyboard, LV_ALIGN_BOTTOM_RIGHT, -10, -55);
  
  // Connect button
  lv_obj_t *connectBtn = lv_btn_create(wifiPopup);
  lv_obj_set_size(connectBtn, 130, 45);
  lv_obj_align(connectBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -5);
  lv_obj_set_style_bg_color(connectBtn, lv_color_hex(0x00AA00), 0);
  lv_obj_t *connectLbl = lv_label_create(connectBtn);
  lv_label_set_text(connectLbl, "Connect");
  lv_obj_set_style_text_font(connectLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(connectLbl);
  lv_obj_add_event_cb(connectBtn, connect_wifi_cb, LV_EVENT_CLICKED, NULL);
}

void openWifiSetup() {
  if (wifiPopup) return;
  
  selectedSSID = "";
  numScannedNetworks = 0;
  wifiPasswordTA = nullptr;
  wifiKeyboard = nullptr;
  pendingNetworkIndex = -1;
  
  wifiPopup = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wifiPopup, 760, 440);
  lv_obj_center(wifiPopup);
  lv_obj_set_style_bg_color(wifiPopup, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(wifiPopup, lv_color_hex(0x0088FF), 0);
  lv_obj_set_style_border_width(wifiPopup, 2, 0);
  lv_obj_set_style_radius(wifiPopup, 10, 0);
  lv_obj_clear_flag(wifiPopup, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *title = lv_label_create(wifiPopup);
  lv_label_set_text(title, LV_SYMBOL_WIFI " WiFi Setup");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
  
  wifiStatusLbl = lv_label_create(wifiPopup);
  lv_label_set_text(wifiStatusLbl, "Scanning...");
  lv_obj_set_style_text_color(wifiStatusLbl, lv_color_hex(0xFFFF00), 0);
  lv_obj_align(wifiStatusLbl, LV_ALIGN_TOP_LEFT, 20, 45);
  
  wifiList = lv_obj_create(wifiPopup);
  lv_obj_set_size(wifiList, 280, 330);
  lv_obj_align(wifiList, LV_ALIGN_TOP_LEFT, 10, 70);
  lv_obj_set_style_bg_color(wifiList, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_pad_all(wifiList, 5, 0);
  lv_obj_set_flex_flow(wifiList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(wifiList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(wifiList, 5, 0);
  
  lv_obj_t *closeBtn = lv_btn_create(wifiPopup);
  lv_obj_set_size(closeBtn, 100, 40);
  lv_obj_align(closeBtn, LV_ALIGN_BOTTOM_LEFT, 20, -5);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x666666), 0);
  lv_obj_t *closeLbl = lv_label_create(closeBtn);
  lv_label_set_text(closeLbl, "Cancel");
  lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(closeLbl);
  lv_obj_add_event_cb(closeBtn, close_wifi_cb, LV_EVENT_CLICKED, NULL);
  
  // Start async scan - must release lock first
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.scanNetworks(true);
  wifiScanInProgress = true;
}

void doWifiConnect() {
  if (pendingNetworkIndex < 0 || pendingNetworkIndex >= numScannedNetworks) return;
  if (!wifiPasswordTA) return;
  
  selectedSSID = String(scannedNetworks[pendingNetworkIndex]);
  const char* password = lv_textarea_get_text(wifiPasswordTA);
  
  prefs.begin("wifi", false);
  prefs.putString("ssid", selectedSSID);
  prefs.putString("pass", password);
  prefs.end();
  
  if (wifiStatusLbl) {
    lv_label_set_text(wifiStatusLbl, "Connecting...");
  }
  
  // Release lock during WiFi connection
  lvgl_port_unlock();
  
  WiFi.disconnect();
  delay(100);
  WiFi.begin(selectedSSID.c_str(), password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(400);
    attempts++;
  }
  
  // Re-acquire lock for UI update
  lvgl_port_lock(-1);
  
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiStatusLbl) lv_label_set_text(wifiStatusLbl, "Connected!");
    pendingCloseWifi = true;
    pendingFetch = true;
    timeClient.begin();
  } else {
    if (wifiStatusLbl) lv_label_set_text(wifiStatusLbl, "Failed! Try again");
  }
}

void createSettingsPopup() {
  if (settingsPopup != nullptr || wifiPopup != nullptr) return;
  
  settingsPopup = lv_obj_create(lv_scr_act());
  lv_obj_set_size(settingsPopup, 700, 380);
  lv_obj_center(settingsPopup);
  lv_obj_set_style_bg_color(settingsPopup, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(settingsPopup, lv_color_hex(0x444444), 0);
  lv_obj_set_style_border_width(settingsPopup, 2, 0);
  lv_obj_set_style_radius(settingsPopup, 10, 0);
  lv_obj_clear_flag(settingsPopup, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *title = lv_label_create(settingsPopup);
  lv_label_set_text(title, "Select Ticker");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
  
  int btnW = 130, btnH = 50;
  int startX = 40, startY = 50;
  int gapX = 145, gapY = 60;
  
  for (int i = 0; i < numTickers; i++) {
    int row = i / 3;
    int col = i % 3;
    
    lv_obj_t *btn = lv_btn_create(settingsPopup);
    lv_obj_set_size(btn, btnW, btnH);
    lv_obj_set_pos(btn, startX + col * gapX, startY + row * gapY);
    
    if (strcmp(tickers[i], currentSymbol.c_str()) == 0) {
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x00AA00), 0);
    } else if (i >= 7) {
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x2255AA), 0);
    } else {
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
    }
    
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, tickers[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl);
    
    lv_obj_add_event_cb(btn, ticker_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  
  // ===== Custom Symbol Input (right side) =====
  lv_obj_t *customLabel = lv_label_create(settingsPopup);
  lv_label_set_text(customLabel, "Custom:");
  lv_obj_set_style_text_font(customLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(customLabel, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_pos(customLabel, 480, 60);
  
  customSymbolTA = lv_textarea_create(settingsPopup);
  lv_obj_set_size(customSymbolTA, 130, 45);
  lv_obj_set_pos(customSymbolTA, 480, 90);
  lv_textarea_set_one_line(customSymbolTA, true);
  lv_textarea_set_max_length(customSymbolTA, 10);
  lv_textarea_set_placeholder_text(customSymbolTA, "SYMBOL");
  lv_obj_set_style_bg_color(customSymbolTA, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_text_color(customSymbolTA, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(customSymbolTA, &lv_font_montserrat_18, 0);
  lv_obj_set_style_border_color(customSymbolTA, lv_color_hex(0x00AA00), 0);
  lv_obj_add_event_cb(customSymbolTA, custom_symbol_ta_cb, LV_EVENT_ALL, NULL);
  
  lv_obj_t *goBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(goBtn, 60, 45);
  lv_obj_set_pos(goBtn, 615, 90);
  lv_obj_set_style_bg_color(goBtn, lv_color_hex(0x00AA00), 0);
  lv_obj_t *goLbl = lv_label_create(goBtn);
  lv_label_set_text(goLbl, "Go");
  lv_obj_set_style_text_font(goLbl, &lv_font_montserrat_18, 0);
  lv_obj_center(goLbl);
  lv_obj_add_event_cb(goBtn, custom_symbol_go_cb, LV_EVENT_CLICKED, NULL);
  
  // ===== Stock Rotation Button =====
  lv_obj_t *rotateBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(rotateBtn, 180, 50);
  lv_obj_set_pos(rotateBtn, 480, 145);
  lv_obj_set_style_bg_color(rotateBtn, lv_color_hex(0x444444), 0);
  lv_obj_t *rotateBtnLbl = lv_label_create(rotateBtn);
  lv_label_set_text(rotateBtnLbl, LV_SYMBOL_REFRESH " Rotation...");
  lv_obj_set_style_text_font(rotateBtnLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(rotateBtnLbl);
  lv_obj_add_event_cb(rotateBtn, [](lv_event_t *e) {
    // Create rotation sub-popup (nearly full screen)
    rotationPopup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(rotationPopup, 760, 440);
    lv_obj_center(rotationPopup);
    lv_obj_set_style_bg_color(rotationPopup, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_color(rotationPopup, lv_color_hex(0x0088FF), 0);
    lv_obj_set_style_border_width(rotationPopup, 2, 0);
    lv_obj_set_style_radius(rotationPopup, 15, 0);
    lv_obj_clear_flag(rotationPopup, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(rotationPopup);
    lv_label_set_text(title, "Stock Rotation Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Enable toggle
    lv_obj_t *enableLabel = lv_label_create(rotationPopup);
    lv_label_set_text(enableLabel, "Enable:");
    lv_obj_set_style_text_font(enableLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(enableLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(enableLabel, 40, 55);
    
    rotationSwitch = lv_switch_create(rotationPopup);
    lv_obj_set_size(rotationSwitch, 70, 35);
    lv_obj_set_pos(rotationSwitch, 130, 50);
    if (rotationEnabled) lv_obj_add_state(rotationSwitch, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(rotationSwitch, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(rotationSwitch, lv_color_hex(0x00AA00), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(rotationSwitch, [](lv_event_t *e) {
      rotationEnabled = lv_obj_has_state(rotationSwitch, LV_STATE_CHECKED);
      prefs.begin("stock", false);
      prefs.putBool("rotate_on", rotationEnabled);
      prefs.end();
      if (rotationEnabled) {
        parseRotationList();
        lastRotationTime = millis();
      }
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Interval dropdown
    lv_obj_t *intervalLabel = lv_label_create(rotationPopup);
    lv_label_set_text(intervalLabel, "Interval:");
    lv_obj_set_style_text_font(intervalLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(intervalLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(intervalLabel, 280, 55);
    
    rotationIntervalDropdown = lv_dropdown_create(rotationPopup);
    lv_dropdown_set_options(rotationIntervalDropdown, "1 min\n2 min\n5 min\n10 min");
    // Set selected based on current interval
    int selectedIdx = 2;  // Default to 5 min
    if (rotationIntervalMins == 1) selectedIdx = 0;
    else if (rotationIntervalMins == 2) selectedIdx = 1;
    else if (rotationIntervalMins == 5) selectedIdx = 2;
    else if (rotationIntervalMins == 10) selectedIdx = 3;
    lv_dropdown_set_selected(rotationIntervalDropdown, selectedIdx);
    lv_obj_set_size(rotationIntervalDropdown, 120, 40);
    lv_obj_set_pos(rotationIntervalDropdown, 370, 48);
    lv_obj_set_style_bg_color(rotationIntervalDropdown, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_text_color(rotationIntervalDropdown, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(rotationIntervalDropdown, lv_color_hex(0x0088FF), 0);
    lv_obj_add_event_cb(rotationIntervalDropdown, [](lv_event_t *e) {
      int sel = lv_dropdown_get_selected(rotationIntervalDropdown);
      int intervals[] = {1, 2, 5, 10};
      rotationIntervalMins = intervals[sel];
      prefs.begin("stock", false);
      prefs.putInt("rotate_int", rotationIntervalMins);
      prefs.end();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Stock list label
    lv_obj_t *listLabel = lv_label_create(rotationPopup);
    lv_label_set_text(listLabel, "Stocks (comma separated):");
    lv_obj_set_style_text_font(listLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(listLabel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(listLabel, 40, 100);
    
    // Text area for stocks - wider
    rotationTA = lv_textarea_create(rotationPopup);
    lv_obj_set_size(rotationTA, 680, 50);
    lv_obj_set_pos(rotationTA, 40, 130);
    lv_textarea_set_one_line(rotationTA, true);
    lv_textarea_set_max_length(rotationTA, 200);
    lv_textarea_set_placeholder_text(rotationTA, "AAPL, MSFT, NVDA, GOOG, TSLA, AMZN, META");
    if (rotationList.length() > 0) lv_textarea_set_text(rotationTA, rotationList.c_str());
    lv_obj_set_style_bg_color(rotationTA, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_text_color(rotationTA, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(rotationTA, &lv_font_montserrat_16, 0);
    lv_obj_set_style_border_color(rotationTA, lv_color_hex(0x0088FF), 0);
    lv_obj_add_event_cb(rotationTA, [](lv_event_t *e) {
      lv_event_code_t code = lv_event_get_code(e);
      if (code == LV_EVENT_FOCUSED) {
        if (rotationKeyboard) lv_obj_clear_flag(rotationKeyboard, LV_OBJ_FLAG_HIDDEN);
      } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
        if (rotationKeyboard) lv_obj_add_flag(rotationKeyboard, LV_OBJ_FLAG_HIDDEN);
      }
    }, LV_EVENT_ALL, NULL);
    
    // Keyboard at bottom of popup - full width
    rotationKeyboard = lv_keyboard_create(rotationPopup);
    lv_obj_set_size(rotationKeyboard, 720, 180);
    lv_obj_align(rotationKeyboard, LV_ALIGN_BOTTOM_MID, 0, -55);
    lv_keyboard_set_textarea(rotationKeyboard, rotationTA);
    lv_obj_add_flag(rotationKeyboard, LV_OBJ_FLAG_HIDDEN);
    
    // Save & Close button
    lv_obj_t *saveBtn = lv_btn_create(rotationPopup);
    lv_obj_set_size(saveBtn, 160, 50);
    lv_obj_align(saveBtn, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(saveBtn, lv_color_hex(0x00AA00), 0);
    lv_obj_t *saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(saveLbl);
    lv_obj_add_event_cb(saveBtn, [](lv_event_t *e) {
      // Save the rotation list
      rotationList = String(lv_textarea_get_text(rotationTA));
      prefs.begin("stock", false);
      prefs.putString("rotate_list", rotationList);
      prefs.end();
      parseRotationList();
      // Close popup
      if (rotationPopup) {
        lv_obj_del(rotationPopup);
        rotationPopup = nullptr;
        rotationTA = nullptr;
        rotationSwitch = nullptr;
        rotationKeyboard = nullptr;
        lv_obj_invalidate(lv_scr_act());  // Force screen refresh
      }
    }, LV_EVENT_CLICKED, NULL);
  }, LV_EVENT_CLICKED, NULL);
  
  // Keyboard (hidden initially, spans full width at bottom)
  customSymbolKeyboard = lv_keyboard_create(settingsPopup);
  lv_obj_set_size(customSymbolKeyboard, 660, 180);
  lv_obj_align(customSymbolKeyboard, LV_ALIGN_BOTTOM_MID, 0, 5);
  lv_keyboard_set_textarea(customSymbolKeyboard, customSymbolTA);
  lv_obj_add_flag(customSymbolKeyboard, LV_OBJ_FLAG_HIDDEN);
  
  lv_obj_t *wifiBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(wifiBtn, 160, 50);
  lv_obj_align(wifiBtn, LV_ALIGN_BOTTOM_LEFT, 20, -15);
  lv_obj_set_style_bg_color(wifiBtn, lv_color_hex(0x0066CC), 0);
  lv_obj_t *wifiLbl = lv_label_create(wifiBtn);
  lv_label_set_text(wifiLbl, LV_SYMBOL_WIFI " WiFi");
  lv_obj_set_style_text_font(wifiLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(wifiLbl);
  lv_obj_add_event_cb(wifiBtn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
  
  // Update Firmware button with version
  lv_obj_t *updateBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(updateBtn, 200, 50);
  lv_obj_align(updateBtn, LV_ALIGN_BOTTOM_MID, 0, -15);
  lv_obj_set_style_bg_color(updateBtn, lv_color_hex(0x8B5CF6), 0);
  lv_obj_t *updateLbl = lv_label_create(updateBtn);
  lv_label_set_text(updateLbl, LV_SYMBOL_DOWNLOAD " Update v" FIRMWARE_VERSION);
  lv_obj_set_style_text_font(updateLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(updateLbl);
  lv_obj_add_event_cb(updateBtn, [](lv_event_t *e) {
    // Trigger GitHub OTA check (runs in loop() to avoid blocking LVGL)
    pendingGitHubOTA = true;
    // Close settings popup
    if (settingsPopup) {
      lv_obj_del(settingsPopup);
      settingsPopup = nullptr;
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *closeBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(closeBtn, 100, 50);
  lv_obj_align(closeBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x666666), 0);
  lv_obj_t *closeLbl = lv_label_create(closeBtn);
  lv_label_set_text(closeLbl, "Close");
  lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(closeLbl);
  lv_obj_add_event_cb(closeBtn, close_popup_cb, LV_EVENT_CLICKED, NULL);
}

// ============ GITHUB OTA UPDATE ============
// Compare version strings like "1.8.0" > "1.7.0"
bool isNewerVersion(const String& remote, const String& local) {
  int rMajor = 0, rMinor = 0, rPatch = 0;
  int lMajor = 0, lMinor = 0, lPatch = 0;
  sscanf(remote.c_str(), "%d.%d.%d", &rMajor, &rMinor, &rPatch);
  sscanf(local.c_str(), "%d.%d.%d", &lMajor, &lMinor, &lPatch);
  if (rMajor != lMajor) return rMajor > lMajor;
  if (rMinor != lMinor) return rMinor > lMinor;
  return rPatch > lPatch;
}

void updateOTAProgress(const char* msg) {
  if (otaProgressLabel) {
    lvgl_port_lock(-1);
    lv_label_set_text(otaProgressLabel, msg);
    lvgl_port_unlock();
  }
  Serial.println(msg);
}

void updateOTAProgressBar(int percent) {
  if (otaProgressBar) {
    lvgl_port_lock(-1);
    lv_bar_set_value(otaProgressBar, percent, LV_ANIM_OFF);
    lvgl_port_unlock();
  }
}

void checkGitHubOTA() {
  // Create progress popup
  lvgl_port_lock(-1);
  otaProgressPopup = lv_obj_create(lv_scr_act());
  lv_obj_set_size(otaProgressPopup, 550, 200);
  lv_obj_center(otaProgressPopup);
  lv_obj_set_style_bg_color(otaProgressPopup, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(otaProgressPopup, lv_color_hex(0x8B5CF6), 0);
  lv_obj_set_style_border_width(otaProgressPopup, 2, 0);
  lv_obj_set_style_radius(otaProgressPopup, 15, 0);
  lv_obj_clear_flag(otaProgressPopup, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *title = lv_label_create(otaProgressPopup);
  lv_label_set_text(title, "Checking for Updates...");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x8B5CF6), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
  
  otaProgressLabel = lv_label_create(otaProgressPopup);
  lv_label_set_text(otaProgressLabel, "Connecting to GitHub...");
  lv_obj_set_style_text_font(otaProgressLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(otaProgressLabel, lv_color_hex(0xC9D1D9), 0);
  lv_obj_align(otaProgressLabel, LV_ALIGN_CENTER, 0, 0);
  
  otaProgressBar = lv_bar_create(otaProgressPopup);
  lv_obj_set_size(otaProgressBar, 400, 20);
  lv_obj_align(otaProgressBar, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_bar_set_value(otaProgressBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(otaProgressBar, lv_color_hex(0x30363D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(otaProgressBar, lv_color_hex(0x8B5CF6), LV_PART_INDICATOR);
  lvgl_port_unlock();
  
  // Check GitHub releases API
  WiFiClientSecure client;
  client.setInsecure();  // Skip certificate verification for simplicity
  
  HTTPClient http;
  String url = "https://api.github.com/repos/" GITHUB_REPO "/releases/latest";
  http.begin(client, url);
  http.addHeader("User-Agent", "ESP32-Stock-Ticker");
  
  int httpCode = http.GET();
  if (httpCode != 200) {
    updateOTAProgress("Failed to check GitHub releases");
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    http.end();
    return;
  }
  
  String payload = http.getString();
  http.end();
  
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    updateOTAProgress("Failed to parse release info");
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    return;
  }
  
  String tagName = doc["tag_name"] | "";
  // Remove 'v' prefix if present
  if (tagName.startsWith("v") || tagName.startsWith("V")) {
    tagName = tagName.substring(1);
  }
  
  char versionMsg[64];
  snprintf(versionMsg, sizeof(versionMsg), "Current: v%s  Latest: v%s", FIRMWARE_VERSION, tagName.c_str());
  updateOTAProgress(versionMsg);
  delay(1500);
  
  if (!isNewerVersion(tagName, FIRMWARE_VERSION)) {
    updateOTAProgress("You're up to date!");
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    return;
  }
  
  // Find firmware.bin in assets
  String firmwareUrl = "";
  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"] | "";
    if (name == "firmware.bin") {
      firmwareUrl = asset["browser_download_url"] | "";
      break;
    }
  }
  
  if (firmwareUrl.length() == 0) {
    updateOTAProgress("No firmware.bin in release");
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    return;
  }
  
  // Download and apply firmware
  updateOTAProgress("Downloading firmware...");
  Serial.println("Downloading: " + firmwareUrl);
  
  // Use fresh client for download
  WiFiClientSecure dlClient;
  dlClient.setInsecure();
  dlClient.setTimeout(60);  // 60 second timeout
  
  HTTPClient dlHttp;
  dlHttp.begin(dlClient, firmwareUrl);
  dlHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  dlHttp.addHeader("User-Agent", "ESP32-Stock-Ticker");
  dlHttp.setTimeout(60000);  // 60 second timeout
  
  httpCode = dlHttp.GET();
  
  if (httpCode != 200) {
    char errMsg[64];
    snprintf(errMsg, sizeof(errMsg), "Download failed: HTTP %d", httpCode);
    updateOTAProgress(errMsg);
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    dlHttp.end();
    return;
  }
  
  int contentLength = dlHttp.getSize();
  Serial.printf("Firmware size: %d bytes\n", contentLength);
  
  if (contentLength <= 0) {
    updateOTAProgress("Invalid firmware size");
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    dlHttp.end();
    return;
  }
  
  if (!Update.begin(contentLength)) {
    updateOTAProgress("Not enough space for update");
    Update.printError(Serial);
    delay(2000);
    lvgl_port_lock(-1);
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
    lvgl_port_unlock();
    dlHttp.end();
    return;
  }
  
  // Create static full-screen overlay to prevent all LVGL updates
  lvgl_port_lock(-1);
  if (otaProgressPopup) {
    lv_obj_del(otaProgressPopup);
    otaProgressPopup = nullptr;
    otaProgressLabel = nullptr;
    otaProgressBar = nullptr;
  }
  
  lv_obj_t *otaOverlay = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(otaOverlay);
  lv_obj_set_size(otaOverlay, 800, 480);
  lv_obj_set_pos(otaOverlay, 0, 0);
  lv_obj_set_style_bg_color(otaOverlay, lv_color_hex(0x0D1117), 0);
  lv_obj_set_style_bg_opa(otaOverlay, LV_OPA_COVER, 0);
  lv_obj_clear_flag(otaOverlay, LV_OBJ_FLAG_SCROLLABLE);
  
  lv_obj_t *otaLabel = lv_label_create(otaOverlay);
  char startMsg[64];
  snprintf(startMsg, sizeof(startMsg), "Updating Firmware...\n\n%d KB to download\n\nPlease wait", contentLength / 1024);
  lv_label_set_text(otaLabel, startMsg);
  lv_obj_set_style_text_font(otaLabel, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(otaLabel, lv_color_hex(0x8B5CF6), 0);
  lv_obj_set_style_text_align(otaLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(otaLabel);
  
  lv_refr_now(NULL);
  lvgl_port_unlock();
  
  otaInProgress = true;
  
  // Download without any UI updates
  WiFiClient *stream = dlHttp.getStreamPtr();
  size_t written = 0;
  uint8_t *buff = (uint8_t*)malloc(2048);
  if (!buff) {
    lvgl_port_lock(-1);
    lv_label_set_text(otaLabel, "Memory allocation failed!");
    lv_refr_now(NULL);
    lvgl_port_unlock();
    delay(3000);
    dlHttp.end();
    ESP.restart();
    return;
  }
  unsigned long lastProgressTime = millis();
  
  while (dlHttp.connected() && written < contentLength) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = min(available, (size_t)2048);
      size_t bytesRead = stream->readBytes(buff, toRead);
      
      if (bytesRead > 0) {
        size_t bytesWritten = Update.write(buff, bytesRead);
        if (bytesWritten != bytesRead) {
          Serial.printf("Write error: %d vs %d\n", bytesWritten, bytesRead);
          break;
        }
        written += bytesWritten;
        lastProgressTime = millis();
        
        // Just log progress to serial, no UI updates
        int percent = (written * 100) / contentLength;
        if (percent % 10 == 0) {
          Serial.printf("OTA: %d%%\n", percent);
        }
      }
    } else {
      delay(1);
    }
    
    // Timeout check - 60 seconds without progress
    if (millis() - lastProgressTime > 60000) {
      Serial.println("Download timeout!");
      break;
    }
  }
  
  dlHttp.end();
  free(buff);
  Serial.printf("Download complete: %d/%d bytes\n", written, contentLength);
  
  if (written == contentLength && Update.end(true)) {
    lvgl_port_lock(-1);
    lv_label_set_text(otaLabel, "Update Complete!\n\nRebooting...");
    lv_refr_now(NULL);
    lvgl_port_unlock();
    delay(1500);
    ESP.restart();
  } else {
    char errMsg[64];
    snprintf(errMsg, sizeof(errMsg), "Update Failed!\n\n%d/%d bytes\n\nRebooting...", written, contentLength);
    lvgl_port_lock(-1);
    lv_label_set_text(otaLabel, errMsg);
    lv_refr_now(NULL);
    lvgl_port_unlock();
    Update.printError(Serial);
    delay(3000);
    ESP.restart();
  }
  otaInProgress = false;
}

// ============ OTA UPDATE WEB SERVER ============
const char* otaPagePart1 = R"rawliteral(
<!DOCTYPE html><html><head><title>Stock Ticker</title>
<style>
body{font-family:Arial;background:#0D1117;color:#C9D1D9;text-align:center;padding:30px}
h1{color:#58A6FF;margin-bottom:5px}h2{color:#8B949E;font-size:18px;margin-top:30px}
.section{background:#161B22;border-radius:10px;padding:20px;margin:15px auto;max-width:400px}
input[type=text]{background:#0D1117;border:1px solid #30363D;color:#C9D1D9;padding:10px;border-radius:6px;width:250px}
input[type=file]{margin:10px}
input[type=submit]{background:#238636;color:#fff;padding:12px 25px;border:none;border-radius:6px;cursor:pointer;margin-top:10px}
input[type=submit]:hover{background:#2EA043}
.version{color:#8B949E;font-size:14px}
</style></head>
<body><h1>Stock Ticker</h1><p class='version'>v)rawliteral" FIRMWARE_VERSION R"rawliteral(</p>
<div class='section'><h2>API Key</h2>
<form method='POST' action='/apikey'>
<input type='text' name='key' placeholder='Enter TwelveData API Key' value=')rawliteral";

const char* otaPagePart2 = R"rawliteral(' maxlength='32'><br>
<input type='submit' value='Save API Key'></form></div>
<div class='section'><h2>Firmware Update</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='update' accept='.bin' required><br>
<input type='submit' value='Upload Firmware'></form></div>
</body></html>
)rawliteral";

void handleOTAUpload() {
  HTTPUpload& upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Done: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void setupOTA() {
  Serial.println("Setting up OTA server...");
  
  if (!MDNS.begin("stockticker")) {
    Serial.println("mDNS failed");
  }
  
  otaServer.on("/", HTTP_GET, []() {
    // Build page with current API key (masked)
    String maskedKey = "";
    if (apiKey.length() > 4) {
      maskedKey = apiKey.substring(0, 4) + "****" + apiKey.substring(apiKey.length() - 4);
    } else if (apiKey.length() > 0) {
      maskedKey = "****";
    }
    String page = String(otaPagePart1) + maskedKey + String(otaPagePart2);
    otaServer.send(200, "text/html", page);
  });
  
  otaServer.on("/apikey", HTTP_POST, []() {
    if (otaServer.hasArg("key")) {
      String newKey = otaServer.arg("key");
      if (newKey.length() > 0) {
        apiKey = newKey;
        Preferences prefs;
        prefs.begin("stock", false);
        prefs.putString("apikey", apiKey);
        prefs.end();
        Serial.println("API key updated via web");
        otaServer.send(200, "text/html", "<html><body style='background:#0D1117;color:#00E676;text-align:center;padding:50px'><h1>API Key Saved!</h1><p><a href='/' style='color:#58A6FF'>Back</a></p></body></html>");
        return;
      }
    }
    otaServer.send(200, "text/html", "<html><body style='background:#0D1117;color:#FF5252;text-align:center;padding:50px'><h1>Invalid Key</h1><p><a href='/' style='color:#58A6FF'>Back</a></p></body></html>");
  });
  
  otaServer.on("/update", HTTP_POST, []() {
    bool success = !Update.hasError();
    otaServer.send(200, "text/html", success ? 
      "<html><body style='background:#0D1117;color:#00E676;text-align:center;padding:50px'><h1>Success! Rebooting...</h1></body></html>" :
      "<html><body style='background:#0D1117;color:#FF5252;text-align:center;padding:50px'><h1>Failed!</h1></body></html>");
    if (success) {
      delay(1000);
      ESP.restart();
    }
  }, handleOTAUpload);
  
  otaServer.begin();
  Serial.println("OTA ready at http://stockticker.local");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Stock Ticker Starting ===");
  
  Board *board = new Board();
  board->init();
  board->begin();
  
  auto *lcd = board->getLCD();
  auto *touch = board->getTouch();
  
  // Init LVGL - this starts the LVGL task
  lvgl_port_init(lcd, touch);
  
  // Lock for initial UI setup
  lvgl_port_lock(-1);
  
  // Dark gradient-style background
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0D1117), 0);
  
  // ===== Left Side Trend Panel =====
  trendPanel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(trendPanel, 145, 340);
  lv_obj_align(trendPanel, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_set_style_bg_color(trendPanel, lv_color_hex(0x161B22), 0);
  lv_obj_set_style_border_color(trendPanel, lv_color_hex(0x00E676), 0);
  lv_obj_set_style_border_width(trendPanel, 2, 0);
  lv_obj_set_style_radius(trendPanel, 12, 0);
  lv_obj_clear_flag(trendPanel, LV_OBJ_FLAG_SCROLLABLE);
  
  // Big trend arrow
  trendArrow = lv_label_create(trendPanel);
  lv_label_set_text(trendArrow, LV_SYMBOL_UP);
  lv_obj_set_style_text_font(trendArrow, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(trendArrow, lv_color_hex(0x00E676), 0);
  lv_obj_align(trendArrow, LV_ALIGN_TOP_MID, 0, 15);
  
  // "Today" label under arrow
  lv_obj_t *todayLabel = lv_label_create(trendPanel);
  lv_label_set_text(todayLabel, "Today");
  lv_obj_set_style_text_font(todayLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(todayLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(todayLabel, LV_ALIGN_TOP_MID, 0, 70);
  
  // Divider line
  lv_obj_t *panelDivider = lv_obj_create(trendPanel);
  lv_obj_set_size(panelDivider, 110, 2);
  lv_obj_align(panelDivider, LV_ALIGN_TOP_MID, 0, 100);
  lv_obj_set_style_bg_color(panelDivider, lv_color_hex(0x30363D), 0);
  lv_obj_set_style_border_width(panelDivider, 0, 0);
  
  // "52 Week Range" title
  lv_obj_t *fiftyTwoTitle = lv_label_create(trendPanel);
  lv_label_set_text(fiftyTwoTitle, "52 Week");
  lv_obj_set_style_text_font(fiftyTwoTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(fiftyTwoTitle, lv_color_hex(0x8B949E), 0);
  lv_obj_align(fiftyTwoTitle, LV_ALIGN_TOP_MID, 0, 115);
  
  // 52-week high label (on top)
  fiftyTwoWeekHighLabel = lv_label_create(trendPanel);
  lv_label_set_text(fiftyTwoWeekHighLabel, "0.00");
  lv_obj_set_style_text_font(fiftyTwoWeekHighLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(fiftyTwoWeekHighLabel, lv_color_hex(0x00E676), 0);
  lv_obj_align(fiftyTwoWeekHighLabel, LV_ALIGN_TOP_MID, 0, 140);
  
  // 52-week range bar (vertical)
  fiftyTwoWeekBar = lv_bar_create(trendPanel);
  lv_obj_set_size(fiftyTwoWeekBar, 24, 120);
  lv_obj_align(fiftyTwoWeekBar, LV_ALIGN_TOP_MID, 0, 162);
  lv_bar_set_range(fiftyTwoWeekBar, 0, 100);
  lv_bar_set_value(fiftyTwoWeekBar, 50, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(fiftyTwoWeekBar, lv_color_hex(0x21262D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(fiftyTwoWeekBar, lv_color_hex(0x00E676), LV_PART_INDICATOR);
  lv_obj_set_style_radius(fiftyTwoWeekBar, 12, LV_PART_MAIN);
  lv_obj_set_style_radius(fiftyTwoWeekBar, 12, LV_PART_INDICATOR);
  
  // 52-week low label (on bottom)
  fiftyTwoWeekLowLabel = lv_label_create(trendPanel);
  lv_label_set_text(fiftyTwoWeekLowLabel, "0.00");
  lv_obj_set_style_text_font(fiftyTwoWeekLowLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(fiftyTwoWeekLowLabel, lv_color_hex(0xFF5252), 0);
  lv_obj_align(fiftyTwoWeekLowLabel, LV_ALIGN_TOP_MID, 0, 290);
  
  // ===== Main content area (shifted right, centered vertically) =====
  // Company name - row 1 (truncate with ... if too long)
  companyNameLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(companyNameLabel, "Loading...");
  lv_obj_set_style_text_font(companyNameLabel, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(companyNameLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_set_width(companyNameLabel, 540);
  lv_label_set_long_mode(companyNameLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(companyNameLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(companyNameLabel, LV_ALIGN_TOP_MID, 70, 8);
  
  // Ticker symbol - row 2
  symbolLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(symbolLabel, currentSymbol.c_str());
  lv_obj_set_style_text_font(symbolLabel, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(symbolLabel, lv_color_hex(0x58A6FF), 0);
  lv_obj_align(symbolLabel, LV_ALIGN_TOP_MID, 70, 50);
  
  // Main price - row 3
  priceLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(priceLabel, "$---.--");
  lv_obj_set_style_text_font(priceLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(priceLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(priceLabel, LV_ALIGN_TOP_MID, 70, 100);
  
  // Container for change values side by side
  lv_obj_t *changeContainer = lv_obj_create(lv_scr_act());
  lv_obj_set_size(changeContainer, 450, 55);
  lv_obj_align(changeContainer, LV_ALIGN_TOP_MID, 70, 160);
  lv_obj_set_style_bg_opa(changeContainer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(changeContainer, 0, 0);
  lv_obj_clear_flag(changeContainer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(changeContainer, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(changeContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(changeContainer, 40, 0);
  
  // Dollar change (left)
  dollarChangeLabel = lv_label_create(changeContainer);
  lv_label_set_text(dollarChangeLabel, "+$0.00");
  lv_obj_set_style_text_font(dollarChangeLabel, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(dollarChangeLabel, lv_color_hex(0x00E676), 0);
  
  // Percent change (right)
  changeLabel = lv_label_create(changeContainer);
  lv_label_set_text(changeLabel, "+0.00%");
  lv_obj_set_style_text_font(changeLabel, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(changeLabel, lv_color_hex(0x00E676), 0);
  
  // ===== Decorative horizontal line =====
  lv_obj_t *hLine = lv_obj_create(lv_scr_act());
  lv_obj_set_size(hLine, 500, 2);
  lv_obj_align(hLine, LV_ALIGN_TOP_MID, 70, 225);
  lv_obj_set_style_bg_color(hLine, lv_color_hex(0x30363D), 0);
  lv_obj_set_style_radius(hLine, 1, 0);
  lv_obj_set_style_border_width(hLine, 0, 0);
  
  // ===== Day Range Bar =====
  lv_obj_t *rangeContainer = lv_obj_create(lv_scr_act());
  lv_obj_set_size(rangeContainer, 500, 45);
  lv_obj_align(rangeContainer, LV_ALIGN_TOP_MID, 70, 235);
  lv_obj_set_style_bg_opa(rangeContainer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rangeContainer, 0, 0);
  lv_obj_clear_flag(rangeContainer, LV_OBJ_FLAG_SCROLLABLE);
  
  // "Day Range" label
  lv_obj_t *dayRangeLabel = lv_label_create(rangeContainer);
  lv_label_set_text(dayRangeLabel, "Day");
  lv_obj_set_style_text_font(dayRangeLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(dayRangeLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(dayRangeLabel, LV_ALIGN_LEFT_MID, 0, 0);
  
  // Low label
  rangeLowLabel = lv_label_create(rangeContainer);
  lv_label_set_text(rangeLowLabel, "0.00");
  lv_obj_set_style_text_font(rangeLowLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(rangeLowLabel, lv_color_hex(0xFF5252), 0);
  lv_obj_align(rangeLowLabel, LV_ALIGN_LEFT_MID, 35, 0);
  
  // Range bar
  rangeBar = lv_bar_create(rangeContainer);
  lv_obj_set_size(rangeBar, 300, 18);
  lv_obj_align(rangeBar, LV_ALIGN_CENTER, 15, 0);
  lv_bar_set_range(rangeBar, 0, 100);
  lv_bar_set_value(rangeBar, 50, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(rangeBar, lv_color_hex(0x21262D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(rangeBar, lv_color_hex(0x00E676), LV_PART_INDICATOR);
  lv_obj_set_style_radius(rangeBar, 9, LV_PART_MAIN);
  lv_obj_set_style_radius(rangeBar, 9, LV_PART_INDICATOR);
  
  // High label
  rangeHighLabel = lv_label_create(rangeContainer);
  lv_label_set_text(rangeHighLabel, "0.00");
  lv_obj_set_style_text_font(rangeHighLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(rangeHighLabel, lv_color_hex(0x00E676), 0);
  lv_obj_align(rangeHighLabel, LV_ALIGN_RIGHT_MID, 0, 0);
  
  // ===== Open/High/Low Info =====
  ohlLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(ohlLabel, "O: --   H: --   L: --");
  lv_obj_set_style_text_font(ohlLabel, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_color(ohlLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(ohlLabel, LV_ALIGN_TOP_MID, 70, 295);
  
  // ===== Volume =====
  volumeLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(volumeLabel, "Vol: --");
  lv_obj_set_style_text_font(volumeLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(volumeLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(volumeLabel, LV_ALIGN_TOP_MID, 70, 330);
  
  // ===== Market Status (now in main area under volume) =====
  marketStatusLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(marketStatusLabel, "Market Closed");
  lv_obj_set_style_text_font(marketStatusLabel, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_color(marketStatusLabel, lv_color_hex(0xFF9800), 0);
  lv_obj_align(marketStatusLabel, LV_ALIGN_TOP_MID, 70, 360);
  
  // Clock display - upper left, subtle
  clockLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(clockLabel, "--:-- --");
  lv_obj_set_style_text_font(clockLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(clockLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(clockLabel, LV_ALIGN_TOP_LEFT, 15, 12);
  
  // ===== Bottom status bar =====
  // WiFi icon - far left
  wifiIcon = lv_label_create(lv_scr_act());
  lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(wifiIcon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(wifiIcon, lv_color_hex(0x484F58), 0);  // Dim until connected
  lv_obj_align(wifiIcon, LV_ALIGN_BOTTOM_LEFT, 10, -15);
  
  // Status/timestamp - right after WiFi icon
  statusLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(statusLabel, "Starting...");
  lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(statusLabel, LV_ALIGN_BOTTOM_LEFT, 35, -16);
  
  // Settings button (smaller)
  lv_obj_t *settingsBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(settingsBtn, 100, 38);
  lv_obj_align(settingsBtn, LV_ALIGN_BOTTOM_RIGHT, -15, -12);
  lv_obj_set_style_bg_color(settingsBtn, lv_color_hex(0x21262D), 0);
  lv_obj_set_style_border_color(settingsBtn, lv_color_hex(0x30363D), 0);
  lv_obj_set_style_border_width(settingsBtn, 1, 0);
  lv_obj_set_style_radius(settingsBtn, 8, 0);
  lv_obj_t *settingsLbl = lv_label_create(settingsBtn);
  lv_label_set_text(settingsLbl, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_font(settingsLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(settingsLbl, lv_color_hex(0xC9D1D9), 0);
  lv_obj_center(settingsLbl);
  lv_obj_add_event_cb(settingsBtn, open_settings_cb, LV_EVENT_CLICKED, NULL);
  
  // Add swipe detection to main screen
  lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
      lv_indev_t *indev = lv_indev_get_act();
      if (indev) {
        lv_indev_get_point(indev, &swipeStart);
        swipeTracking = true;
      }
    } else if (code == LV_EVENT_RELEASED && swipeTracking) {
      swipeTracking = false;
      lv_indev_t *indev = lv_indev_get_act();
      if (indev && rotationEnabled && rotationCount > 1 && settingsPopup == nullptr) {
        lv_point_t swipeEnd;
        lv_indev_get_point(indev, &swipeEnd);
        int dx = swipeEnd.x - swipeStart.x;
        if (abs(dx) > 100) {  // Minimum swipe distance
          if (dx < 0) {
            // Swipe left - next stock
            rotationIndex = (rotationIndex + 1) % rotationCount;
          } else {
            // Swipe right - previous stock
            rotationIndex = (rotationIndex - 1 + rotationCount) % rotationCount;
          }
          currentSymbol = rotationSymbols[rotationIndex];
          lastRotationTime = millis();  // Reset rotation timer
          pendingFetch = true;
        }
      }
    }
  }, LV_EVENT_ALL, NULL);
  
  lvgl_port_unlock();
  
  // Load saved data
  prefs.begin("stock", true);
  currentSymbol = prefs.getString("symbol", "MSFT");
  lastPrice = prefs.getString("price", "N/A");
  // Load API key - fall back to compiled key if not saved
  apiKey = prefs.getString("apikey", "");
  if (apiKey.length() == 0) {
    apiKey = TWELVEDATA_API_KEY;  // Fall back to compiled config
  }
  prefs.end();
  
  Serial.printf("API Key loaded: %s***\n", apiKey.substring(0, 4).c_str());
  
  if (lvgl_port_lock(100)) {
    lv_label_set_text(symbolLabel, currentSymbol.c_str());
    lv_label_set_text(priceLabel, lastPrice.c_str());
    lvgl_port_unlock();
  }
  
  // Try WiFi
  prefs.begin("wifi", true);
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();
  
  if (savedSSID.length() > 0) {
    if (lvgl_port_lock(100)) {
      lv_label_set_text(statusLabel, "Connecting WiFi...");
      lvgl_port_unlock();
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi connected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      
      if (lvgl_port_lock(100)) {
        lv_label_set_text(statusLabel, "Connected");
        lvgl_port_unlock();
      }
      timeClient.begin();
      
      // Load rotation settings
      prefs.begin("stock", true);
      rotationEnabled = prefs.getBool("rotate_on", false);
      rotationList = prefs.getString("rotate_list", "");
      rotationIntervalMins = prefs.getInt("rotate_int", 5);
      prefs.end();
      parseRotationList();
      lastRotationTime = millis();
      
      // If rotation enabled, start with first symbol
      if (rotationEnabled && rotationCount > 0) {
        currentSymbol = rotationSymbols[0];
        rotationIndex = 0;
      }
      
      fetchPrice();
      
      // Start OTA web server
      delay(500);
      setupOTA();
    } else {
      if (lvgl_port_lock(100)) {
        lv_label_set_text(statusLabel, "WiFi Failed - tap Settings");
        lvgl_port_unlock();
      }
    }
  } else {
    if (lvgl_port_lock(100)) {
      lv_label_set_text(statusLabel, "No WiFi - tap Settings");
      lvgl_port_unlock();
    }
  }
  
  Serial.println("Setup complete!");
}

void loop() {
  // Don't call lv_timer_handler() - the LVGL task handles it
  
  // Process pending actions with proper locking
  if (pendingOpenSettings || pendingClosePopup || pendingOpenWifi || 
      pendingCloseWifi || pendingShowKeyboard || pendingWifiConnect ||
      pendingTickerIndex >= 0 || pendingFetch || pendingCustomSymbol) {
    
    if (lvgl_port_lock(50)) {
      
      // Settings popup
      if (pendingOpenSettings) {
        pendingOpenSettings = false;
        if (settingsPopup == nullptr && wifiPopup == nullptr) {
          createSettingsPopup();
        }
      }
      
      if (pendingClosePopup) {
        pendingClosePopup = false;
        // First close rotation popup if open
        if (rotationPopup != nullptr) {
          lv_obj_del(rotationPopup);
          rotationPopup = nullptr;
        }
        if (settingsPopup != nullptr) {
          lv_obj_del(settingsPopup);
          settingsPopup = nullptr;
          customSymbolTA = nullptr;
          customSymbolKeyboard = nullptr;
          rotationTA = nullptr;
          rotationSwitch = nullptr;
          rotationKeyboard = nullptr;
          lv_obj_invalidate(lv_scr_act());  // Force full screen refresh
        }
      }
      
      // WiFi popup
      if (pendingOpenWifi) {
        pendingOpenWifi = false;
        if (wifiPopup == nullptr) {
          openWifiSetup();
        }
      }
      
      if (pendingCloseWifi) {
        pendingCloseWifi = false;
        if (wifiPopup != nullptr) {
          lv_obj_del(wifiPopup);
          wifiPopup = nullptr;
          wifiList = nullptr;
          wifiPasswordTA = nullptr;
          wifiKeyboard = nullptr;
          wifiStatusLbl = nullptr;
          wifiScanInProgress = false;
          pendingNetworkIndex = -1;
        }
      }
      
      // Network selection / keyboard
      if (pendingShowKeyboard && pendingNetworkIndex >= 0) {
        pendingShowKeyboard = false;
        showWifiKeyboard();
      }
      
      // WiFi connect
      if (pendingWifiConnect) {
        pendingWifiConnect = false;
        doWifiConnect();
        // doWifiConnect handles its own unlock/lock
      }
      
      // Ticker change
      if (pendingTickerIndex >= 0) {
        currentSymbol = tickers[pendingTickerIndex];
        pendingTickerIndex = -1;
        pendingFetch = true;
        
        String display = currentSymbol + " $---.--";
        lv_label_set_text(priceLabel, display.c_str());
        lv_label_set_text(statusLabel, "Loading...");
      }
      
      // Custom symbol change
      if (pendingCustomSymbol) {
        currentSymbol = pendingCustomSymbolStr;
        pendingCustomSymbol = false;
        pendingCustomSymbolStr = "";
        pendingFetch = true;
        
        String display = currentSymbol + " $---.--";
        lv_label_set_text(priceLabel, display.c_str());
        lv_label_set_text(statusLabel, "Loading...");
      }
      
      lvgl_port_unlock();;
    }
  }
  
  // Handle fetch outside of LVGL lock
  if (pendingFetch) {
    pendingFetch = false;
    fetchPrice();
  }
  
  // Handle GitHub OTA check outside of LVGL lock
  if (pendingGitHubOTA) {
    pendingGitHubOTA = false;
    checkGitHubOTA();
  }
  
  // Check async WiFi scan
  if (wifiScanInProgress) {
    int result = WiFi.scanComplete();
    if (result >= 0) {
      wifiScanInProgress = false;
      numScannedNetworks = 0;
      
      for (int i = 0; i < result && numScannedNetworks < 10; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() > 0) {
          strncpy(scannedNetworks[numScannedNetworks], ssid.c_str(), 32);
          scannedNetworks[numScannedNetworks][32] = '\0';
          numScannedNetworks++;
        }
      }
      WiFi.scanDelete();
      
      if (lvgl_port_lock(100)) {
        if (wifiStatusLbl) {
          char buf[32];
          snprintf(buf, sizeof(buf), "Found %d networks", numScannedNetworks);
          lv_label_set_text(wifiStatusLbl, buf);
        }
        buildWifiNetworkList();
        lvgl_port_unlock();
      }
    } else if (result == WIFI_SCAN_FAILED) {
      wifiScanInProgress = false;
      if (lvgl_port_lock(100)) {
        if (wifiStatusLbl) lv_label_set_text(wifiStatusLbl, "Scan failed");
        lvgl_port_unlock();
      }
    }
  }
  
  // Stock rotation - based on user-selected interval
  if (rotationEnabled && rotationCount > 1 && settingsPopup == nullptr) {
    uint32_t intervalMs = (uint32_t)rotationIntervalMins * 60000;
    if (millis() - lastRotationTime > intervalMs) {
      lastRotationTime = millis();
      int nextIndex = (rotationIndex + 1) % rotationCount;
      String nextSymbol = rotationSymbols[nextIndex];
      
      // Prefetch the next stock's data BEFORE visual transition
      Serial.printf("Prefetching data for %s...\n", nextSymbol.c_str());
      bool fetched = prefetchStockData(nextSymbol);
      
      if (fetched) {
        rotationIndex = nextIndex;
        
        // Now do smooth visual transition with data ready
        if (lvgl_port_lock(100)) {
          // Fade out
          lv_obj_set_style_opa(priceLabel, LV_OPA_0, 0);
          lv_obj_set_style_opa(companyNameLabel, LV_OPA_0, 0);
          lv_obj_set_style_opa(symbolLabel, LV_OPA_0, 0);
          lv_obj_set_style_opa(changeLabel, LV_OPA_0, 0);
          lv_obj_set_style_opa(dollarChangeLabel, LV_OPA_0, 0);
          lv_obj_set_style_opa(ohlLabel, LV_OPA_0, 0);
          lv_obj_set_style_opa(volumeLabel, LV_OPA_0, 0);
          lvgl_port_unlock();
        }
        
        delay(100);  // Brief fade out
        
        // Apply prefetched data and fade in
        if (lvgl_port_lock(100)) {
          applyPrefetchedData();  // Paint all data at once
          
          // Fade back in
          lv_obj_set_style_opa(priceLabel, LV_OPA_COVER, 0);
          lv_obj_set_style_opa(companyNameLabel, LV_OPA_COVER, 0);
          lv_obj_set_style_opa(symbolLabel, LV_OPA_COVER, 0);
          lv_obj_set_style_opa(changeLabel, LV_OPA_COVER, 0);
          lv_obj_set_style_opa(dollarChangeLabel, LV_OPA_COVER, 0);
          lv_obj_set_style_opa(ohlLabel, LV_OPA_COVER, 0);
          lv_obj_set_style_opa(volumeLabel, LV_OPA_COVER, 0);
          lv_obj_invalidate(lv_scr_act());
          lvgl_port_unlock();
        }
        Serial.printf("Rotated to %s\n", nextSymbol.c_str());
      } else {
        Serial.println("Prefetch failed, skipping rotation");
      }
    }
  }
  
  // Periodic refresh logic
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  
  if (isMarketOpen) {
    // Market open: refresh every 5 minutes
    if (now - lastCheck > 300000) {
      lastCheck = now;
      if (WiFi.status() == WL_CONNECTED && !rotationEnabled) {
        fetchPrice();
      }
    }
  } else {
    // Market closed: smart check interval
    // - Every 5 minutes near market open (9:00-10:00 AM) or close (3:30-4:30 PM)
    // - Every hour otherwise (nights/weekends)
    uint32_t checkInterval = isNearMarketTransition() ? MARKET_TRANSITION_CHECK_INTERVAL : MARKET_CLOSED_CHECK_INTERVAL;
    
    if (now - lastMarketCheck > checkInterval) {
      lastMarketCheck = now;
      if (WiFi.status() == WL_CONNECTED) {
        if (isNearMarketTransition()) {
          Serial.println("Near market transition - checking every 5 min");
        } else {
          Serial.println("Market closed - hourly check");
        }
        fetchPrice();  // This will update isMarketOpen if market opened
      }
    }
  }
  
  // Handle OTA web server requests
  otaServer.handleClient();
  
  // Update clock display every second
  static uint32_t lastClockUpdate = 0;
  if (millis() - lastClockUpdate > 1000) {
    lastClockUpdate = millis();
    if (WiFi.status() == WL_CONNECTED && clockLabel) {
      timeClient.update();
      int hours = timeClient.getHours();
      int mins = timeClient.getMinutes();
      const char* ampm = hours >= 12 ? "PM" : "AM";
      hours = hours % 12;
      if (hours == 0) hours = 12;
      char clockBuf[16];
      snprintf(clockBuf, sizeof(clockBuf), "%d:%02d %s", hours, mins, ampm);
      if (lvgl_port_lock(50)) {
        lv_label_set_text(clockLabel, clockBuf);
        lvgl_port_unlock();
      }
    }
  }
  
  delay(10);
}
