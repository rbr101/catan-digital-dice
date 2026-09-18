#include "WebPage.h"

/**
 * Connect to WiFi network
 *
 * Attempts to connect to the specified WiFi network and waits until
 * the connection is established. Prints connection status to Serial.
 *
 * @param WIFI_SSID Network name
 * @param WIFI_PASS Network password
 */
void connectWifi(const char *WIFI_SSID, const char *WIFI_PASS)
{
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());

    // Reset any stale state left over from a previous connection/session
    // (e.g. after flashing new firmware) before starting a fresh attempt.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(100);

    // Disable WiFi modem sleep: it saves power but adds tens to hundreds of
    // ms of latency to every request and can make the connection flaky.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Wait for connection, but give up after ~20s so a wrong SSID/password
    // doesn't hang here forever with nothing but dots.
    unsigned long connectStart = millis();
    const unsigned long connectTimeoutMs = 20000;
    while (WiFi.status() != WL_CONNECTED && millis() - connectStart < connectTimeoutMs)
    {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.print("\nWiFi connection FAILED, status code: ");
        Serial.println(WiFi.status());
        Serial.println("Will keep retrying in the background (see loop()).");
        return;
    }

    // Print success message and IP address
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

/**
 * Read HTML content from SPIFFS
 *
 * Loads index.html from the SPIFFS filesystem and sets up
 * routes for static files (CSS, JavaScript).
 *
 * @param htmlPage String reference to store the loaded HTML content
 * @param server WebServer instance to configure static routes
 */
void readHtml(String &htmlPage, WebServer &server)
{
    // Initialize SPIFFS if not already mounted
    if (!SPIFFS.begin(true))
    {
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }

    // Open the index.html file
    File file = SPIFFS.open("/index.html", "r");
    if (!file)
    {
        Serial.println("Failed to open file for reading");
        return;
    }

    // Read the file contents into the htmlPage string
    htmlPage = file.readString();
    file.close();

    // Set up routes for static assets (CSS and JavaScript files)
    server.serveStatic("/css/", SPIFFS, "/css/");
    server.serveStatic("/js/", SPIFFS, "/js/");
}