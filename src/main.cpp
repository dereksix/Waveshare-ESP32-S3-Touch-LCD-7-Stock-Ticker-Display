// Stock Ticker for ESP32-S3 with 7" Touch Display
// Displays real-time stock quotes using TwelveData API
// Hardware: Waveshare ESP32-S3-Touch-LCD-7 (800x480)
// 
// IMPORTANT: Copy include/config.example.h to include/config.h and add your API key
// LVGL port runs its own task, so we must use lvgl_port_lock/unlock

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "config.h"

using namespace esp_panel::board;

Preferences prefs;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000, 60000);

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

// API Key - loaded from config.h
const char* twelvedataKey = TWELVEDATA_API_KEY;

// Tickers
const char* tickers[] = {"MSFT", "AAPL", "GOOGL", "AMZN", "NVDA", "TSLA", "META", "SPY", "QQQ"};
const int numTickers = 9;

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
  String url = "https://api.twelvedata.com/quote?symbol=" + currentSymbol + "&apikey=" + twelvedataKey;
  
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
      // Update ticker with company name (Company Name - TICKER format)
      char symbolWithName[80];
      if (companyName.length() > 0) {
        snprintf(symbolWithName, sizeof(symbolWithName), "%s - $%s", companyName.c_str(), currentSymbol.c_str());
      } else {
        snprintf(symbolWithName, sizeof(symbolWithName), "$%s", currentSymbol.c_str());
      }
      lv_label_set_text(symbolLabel, symbolWithName);
      
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
    
    prefs.begin("stock", false);
    prefs.putString("symbol", currentSymbol);
    prefs.putString("price", lastPrice);
    prefs.end();
  } else {
    if (lvgl_port_lock(100)) {
      lv_label_set_text(statusLabel, "API Error");
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
  
  // Keyboard (hidden initially, spans full width at bottom)
  customSymbolKeyboard = lv_keyboard_create(settingsPopup);
  lv_obj_set_size(customSymbolKeyboard, 660, 180);
  lv_obj_align(customSymbolKeyboard, LV_ALIGN_BOTTOM_MID, 0, 5);
  lv_keyboard_set_textarea(customSymbolKeyboard, customSymbolTA);
  lv_obj_add_flag(customSymbolKeyboard, LV_OBJ_FLAG_HIDDEN);
  
  lv_obj_t *wifiBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(wifiBtn, 180, 50);
  lv_obj_align(wifiBtn, LV_ALIGN_BOTTOM_LEFT, 20, -15);
  lv_obj_set_style_bg_color(wifiBtn, lv_color_hex(0x0066CC), 0);
  lv_obj_t *wifiLbl = lv_label_create(wifiBtn);
  lv_label_set_text(wifiLbl, LV_SYMBOL_WIFI " WiFi Setup");
  lv_obj_set_style_text_font(wifiLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(wifiLbl);
  lv_obj_add_event_cb(wifiBtn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *closeBtn = lv_btn_create(settingsPopup);
  lv_obj_set_size(closeBtn, 120, 50);
  lv_obj_align(closeBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(0x666666), 0);
  lv_obj_t *closeLbl = lv_label_create(closeBtn);
  lv_label_set_text(closeLbl, "Close");
  lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(closeLbl);
  lv_obj_add_event_cb(closeBtn, close_popup_cb, LV_EVENT_CLICKED, NULL);
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
  // Ticker symbol with company name - big font like price
  symbolLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(symbolLabel, currentSymbol.c_str());
  lv_obj_set_style_text_font(symbolLabel, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(symbolLabel, lv_color_hex(0x58A6FF), 0);
  lv_obj_set_width(symbolLabel, 540);
  lv_label_set_long_mode(symbolLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(symbolLabel, LV_ALIGN_TOP_MID, 70, 10);
  
  // Main price (pushed down for ~0.75" gap from company name)
  priceLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(priceLabel, "$---.--");
  lv_obj_set_style_text_font(priceLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(priceLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(priceLabel, LV_ALIGN_TOP_MID, 70, 85);
  
  // Container for change values side by side
  lv_obj_t *changeContainer = lv_obj_create(lv_scr_act());
  lv_obj_set_size(changeContainer, 450, 55);
  lv_obj_align(changeContainer, LV_ALIGN_TOP_MID, 70, 145);
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
  lv_obj_align(hLine, LV_ALIGN_TOP_MID, 70, 210);
  lv_obj_set_style_bg_color(hLine, lv_color_hex(0x30363D), 0);
  lv_obj_set_style_radius(hLine, 1, 0);
  lv_obj_set_style_border_width(hLine, 0, 0);
  
  // ===== Day Range Bar =====
  lv_obj_t *rangeContainer = lv_obj_create(lv_scr_act());
  lv_obj_set_size(rangeContainer, 500, 45);
  lv_obj_align(rangeContainer, LV_ALIGN_TOP_MID, 70, 220);
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
  lv_obj_align(ohlLabel, LV_ALIGN_TOP_MID, 70, 280);
  
  // ===== Volume =====
  volumeLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(volumeLabel, "Vol: --");
  lv_obj_set_style_text_font(volumeLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(volumeLabel, lv_color_hex(0x8B949E), 0);
  lv_obj_align(volumeLabel, LV_ALIGN_TOP_MID, 70, 310);
  
  // ===== Market Status (now in main area under volume) =====
  marketStatusLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(marketStatusLabel, "Market Closed");
  lv_obj_set_style_text_font(marketStatusLabel, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_color(marketStatusLabel, lv_color_hex(0xFF9800), 0);
  lv_obj_align(marketStatusLabel, LV_ALIGN_TOP_MID, 70, 345);
  
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
  
  // Settings button
  lv_obj_t *settingsBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(settingsBtn, 130, 45);
  lv_obj_align(settingsBtn, LV_ALIGN_BOTTOM_RIGHT, -25, -20);
  lv_obj_set_style_bg_color(settingsBtn, lv_color_hex(0x21262D), 0);
  lv_obj_set_style_border_color(settingsBtn, lv_color_hex(0x30363D), 0);
  lv_obj_set_style_border_width(settingsBtn, 1, 0);
  lv_obj_set_style_radius(settingsBtn, 8, 0);
  lv_obj_t *settingsLbl = lv_label_create(settingsBtn);
  lv_label_set_text(settingsLbl, LV_SYMBOL_SETTINGS " Settings");
  lv_obj_set_style_text_font(settingsLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(settingsLbl, lv_color_hex(0xC9D1D9), 0);
  lv_obj_center(settingsLbl);
  lv_obj_add_event_cb(settingsBtn, open_settings_cb, LV_EVENT_CLICKED, NULL);
  
  lvgl_port_unlock();
  
  // Load saved data
  prefs.begin("stock", true);
  currentSymbol = prefs.getString("symbol", "MSFT");
  lastPrice = prefs.getString("price", "N/A");
  prefs.end();
  
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
      if (lvgl_port_lock(100)) {
        lv_label_set_text(statusLabel, "Connected");
        lvgl_port_unlock();
      }
      timeClient.begin();
      fetchPrice();
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
        if (settingsPopup != nullptr) {
          lv_obj_del(settingsPopup);
          settingsPopup = nullptr;
          customSymbolTA = nullptr;
          customSymbolKeyboard = nullptr;
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
  
  // Periodic refresh - only when market is open
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 300000) {  // 5 minutes
    lastCheck = millis();
    if (WiFi.status() == WL_CONNECTED && isMarketOpen) {
      fetchPrice();
    }
  }
  
  delay(10);
}
