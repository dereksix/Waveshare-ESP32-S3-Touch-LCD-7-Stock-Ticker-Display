# ESP32 Stock Ticker

A real-time stock ticker display for the **Waveshare ESP32-S3-Touch-LCD-7** (800x480 7" touchscreen). Shows live stock quotes with a beautiful dark theme UI.

![Stock Ticker Display](docs/screenshot.png)

## Features

- 📈 **Real-time stock quotes** via TwelveData API
- 📱 **Touch-enabled settings** - tap to change stocks or configure WiFi
- 🎨 **Modern dark theme UI** with trend indicators
- 📊 **Comprehensive data display**:
  - Current price with dollar and percent change
  - Day range with visual progress bar
  - Open/High/Low prices
  - Volume
  - 52-week range indicator
  - Market open/closed status
- ⚡ **Smart refresh** - only fetches data when market is open (saves API calls)
- 🔧 **Custom symbol input** - enter any stock symbol via on-screen keyboard
- 📶 **WiFi configuration** - scan and connect to networks via touch UI
- 💾 **Persistent settings** - remembers your stock and WiFi across reboots

## Hardware Requirements

- **Waveshare ESP32-S3-Touch-LCD-7** (800x480 RGB LCD with GT911 touch)
  - [Product Page](https://www.waveshare.com/esp32-s3-touch-lcd-7.htm)
- USB-C cable for programming and power
- 5V power supply (USB or external)

## Software Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- [TwelveData API Key](https://twelvedata.com/) (free tier available - 800 calls/day)

## Installation

### 1. Clone the Repository

```bash
git clone https://github.com/YOUR_USERNAME/esp32-stock-ticker.git
cd esp32-stock-ticker
```

### 2. Configure Your API Key

Copy the example config file and add your TwelveData API key:

```bash
cp include/config.example.h include/config.h
```

Edit `include/config.h` and replace `YOUR_API_KEY_HERE` with your actual API key:

```cpp
#define TWELVEDATA_API_KEY "your_actual_api_key_here"
```

> ⚠️ **Important**: Never commit `config.h` to git - it contains your private API key!

### 3. Build and Upload

Open the project in VS Code with PlatformIO, then:

1. Connect your ESP32-S3 board via USB
2. Click the PlatformIO Upload button (→) or run:

```bash
pio run -t upload
```

## Usage

### First Boot

1. The display will show "No WiFi" initially
2. Tap the **Settings** button (bottom right)
3. Tap **WiFi Setup** to scan for networks
4. Select your network and enter the password
5. Once connected, stock data will load automatically

### Changing Stocks

1. Tap **Settings**
2. Either:
   - Tap one of the preset stock buttons (MSFT, AAPL, GOOGL, etc.)
   - Type a custom symbol using the **Custom** input field and tap **Go**
3. The popup closes and data loads for the new symbol

### Preset Stocks

The following stocks are available as quick-select buttons:
- Tech: MSFT, AAPL, GOOGL, AMZN, NVDA, TSLA, META
- ETFs: SPY, QQQ

## Project Structure

```
├── include/
│   ├── config.example.h    # Template for API key config
│   ├── config.h            # Your API key (git-ignored)
│   └── lv_conf.h           # LVGL configuration
├── lib/
│   ├── ArduinoJson/        # JSON parsing
│   ├── ESP32_Display_Panel/ # Display driver
│   ├── lvgl/               # Graphics library
│   ├── NTPClient/          # Time sync
│   └── WiFiManager/        # WiFi utilities
├── src/
│   ├── main.cpp            # Main application
│   ├── lvgl_v8_port.cpp    # LVGL display/touch integration
│   └── lvgl_v8_port.h
└── platformio.ini          # Build configuration
```

## API Information

This project uses the [TwelveData API](https://twelvedata.com/) for stock quotes.

- **Free tier**: 800 API calls/day, 8 calls/minute
- **Endpoint used**: `/quote` for real-time data
- **Smart refresh**: Only refreshes every 5 minutes when market is open

### API Response Data Used

- `close` - Current/last price
- `previous_close` - For calculating change
- `open`, `high`, `low` - Day's OHLC
- `volume` - Trading volume
- `fifty_two_week.high/low` - 52-week range
- `is_market_open` - Market status
- `name` - Company name

## Customization

### Adding More Preset Stocks

Edit the `tickers` array in `main.cpp`:

```cpp
const char* tickers[] = {"MSFT", "AAPL", "GOOGL", "AMZN", "NVDA", "TSLA", "META", "SPY", "QQQ"};
const int numTickers = 9;
```

### Changing Refresh Interval

Modify the timing constant in the `loop()` function (currently 5 minutes):

```cpp
if (now - lastRefresh >= 300000 && isMarketOpen) {  // 300000ms = 5 minutes
```

### Timezone Adjustment

The NTP client is configured for Eastern Time (UTC-5). Modify in `main.cpp`:

```cpp
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000, 60000);  // -18000 = UTC-5 hours
```

## Troubleshooting

### Display is blank
- Check USB connection and power
- Try pressing the reset button on the board

### "No WiFi" message
- Tap Settings → WiFi Setup to configure
- Ensure your network is 2.4GHz (ESP32 doesn't support 5GHz)

### Stock data not loading
- Check your API key in `config.h`
- Verify WiFi is connected (green WiFi icon)
- Check TwelveData API quota (800 calls/day on free tier)

### Touch not responding
- The GT911 touch controller may need a moment after boot
- Try power cycling the device

## License

MIT License - See [LICENSE](LICENSE) file

## Credits

- [LVGL](https://lvgl.io/) - Graphics library
- [TwelveData](https://twelvedata.com/) - Stock data API
- [Waveshare](https://www.waveshare.com/) - Hardware
- [ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) - Display driver

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Submit a pull request

---

Made with ❤️ for makers and traders
