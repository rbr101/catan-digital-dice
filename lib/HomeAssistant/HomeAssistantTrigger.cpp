/**
 * HomeAssistantTrigger.cpp
 *
 * Implementation of the Home Assistant integration for the Catan board generator.
 * This file provides functionality to notify a Home Assistant instance
 * about dice rolls and number selections during Catan gameplay.
 *
 * The integration uses the Home Assistant webhook API to send notifications
 * when numbers are selected, enabling home automation actions
 * to be triggered by game events.
 *
 * Whether the integration is enabled, and its host/port/token, are runtime
 * settings stored in NVS flash (via Preferences) so they survive reboots and
 * can be changed from the web UI without reflashing.
 */

#include "HomeAssistantTrigger.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>

// Static variables to store Home Assistant connection configuration
static String _haHost;           // Hostname or IP address of Home Assistant instance
static uint16_t _haPort;         // Port number (typically 8123)
static String _haApiKey;         // Long-lived access token for authentication
static String _haScriptEndpoint; // API endpoint for triggering automation scripts
static bool _haEnabled = false;  // Whether triggers are actually sent

static Preferences haPrefs;
static const char *HA_NAMESPACE = "haconfig";

void initHomeAssistant(const char *host, uint16_t port, const char *apiKey, const char *scriptEndpoint)
{
    // Store the configuration in static variables for later use
    _haHost = host;
    _haPort = port;
    _haApiKey = apiKey;
    _haScriptEndpoint = scriptEndpoint;
}

void setHomeAssistantEnabled(bool enabled)
{
    _haEnabled = enabled;
}

bool isHomeAssistantEnabled()
{
    return _haEnabled;
}

String getHomeAssistantHost()
{
    return _haHost;
}

uint16_t getHomeAssistantPort()
{
    return _haPort;
}

String getHomeAssistantToken()
{
    return _haApiKey;
}

void loadHomeAssistantConfig(const char *defaultHost, uint16_t defaultPort, const char *defaultToken, bool defaultEnabled)
{
    haPrefs.begin(HA_NAMESPACE, true); // read-only
    String host = haPrefs.getString("host", defaultHost);
    uint16_t port = haPrefs.getUShort("port", defaultPort);
    String token = haPrefs.getString("token", defaultToken);
    bool enabled = haPrefs.getBool("enabled", defaultEnabled);
    haPrefs.end();

    initHomeAssistant(host.c_str(), port, token.c_str(), "/api/services/script/turn_on");
    setHomeAssistantEnabled(enabled);
}

void configureHomeAssistant(const String &host, uint16_t port, const String &token, bool enabled)
{
    initHomeAssistant(host.c_str(), port, token.c_str(), "/api/services/script/turn_on");
    setHomeAssistantEnabled(enabled);

    haPrefs.begin(HA_NAMESPACE, false); // read-write
    haPrefs.putString("host", host);
    haPrefs.putUShort("port", port);
    haPrefs.putString("token", token);
    haPrefs.putBool("enabled", enabled);
    haPrefs.end();
}

/**
 * Trigger a Home Assistant automation when a number is selected
 *
 * Sends an HTTP POST request to the configured Home Assistant instance
 * with the selected dice number. This can be used to trigger automations
 * like lighting effects corresponding to different game events.
 *
 * Uses a webhook endpoint format: http://{host}:{port}/api/webhook/esp32_number
 * with a JSON payload containing the selected number. Does nothing if the
 * integration is currently disabled.
 *
 * @param selectedNumber The dice number that was selected (2-12, or 7 for robber)
 */
void triggerHomeAssistantScript(int selectedNumber)
{
    if (!_haEnabled)
    {
        return;
    }

    HTTPClient http;

    // Construct the URL for the Home Assistant webhook
    // This uses a webhook endpoint called "esp32_number"
    String url = "http://" + _haHost + ":" + String(_haPort) + "/api/webhook/esp32_number";

    // Initialize the HTTP request
    http.begin(url);

    // Keep a misconfigured/unreachable Home Assistant from blocking the
    // caller (and therefore every web UI request) for the default ~5s.
    http.setConnectTimeout(1500);
    http.setTimeout(1500);

    // Set required headers
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + _haApiKey);

    // Create a simple JSON payload with the selected number
    String payload = "{\"selectedNumber\": " + String(selectedNumber) + "}";

    // Send the POST request
    int httpResponseCode = http.POST(payload);

    // Log the result
    if (httpResponseCode > 0)
    {
        Serial.printf("HomeAssistant trigger response code: %d\n", httpResponseCode);
    }
    else
    {
        Serial.printf("Error triggering HomeAssistant: %s\n", http.errorToString(httpResponseCode).c_str());
    }

    // Clean up HTTP resources
    http.end();
}
