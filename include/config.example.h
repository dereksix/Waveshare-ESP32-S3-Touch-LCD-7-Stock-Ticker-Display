// config.example.h - Example configuration file
// Copy this file to config.h and add your API key

#ifndef CONFIG_H
#define CONFIG_H

// Get your free API key from https://twelvedata.com/
#define TWELVEDATA_API_KEY "YOUR_API_KEY_HERE"

// Finnhub API key - PRIMARY for quotes (60 calls/min)
// Get your free API key from https://finnhub.io/
#define FINNHUB_API_KEY "YOUR_FINNHUB_KEY_HERE"

// Polygon API key - fallback for quotes (5 calls/min)
// Get your free API key from https://polygon.io/
#define POLYGON_API_KEY "YOUR_POLYGON_KEY_HERE"

// P2P Network Configuration (optional - share stock data between devices)
// This reduces TwelveData API calls by sharing cached data across devices
// Set up your own P2P registry at: https://github.com/dereksix/stock-ticker-p2p-registry
#define P2P_ENABLED false
#define P2P_REGISTRY_URL "https://your-registry.workers.dev"
#define P2P_NETWORK_KEY "your-network-secret"

#endif
