/**
 * HomeAssistantTrigger.h
 *
 * This header defines the interface for integrating with Home Assistant,
 * a popular open-source home automation platform. The integration allows
 * the Catan board to notify Home Assistant when dice numbers are selected,
 * enabling home automation responses to game events.
 *
 * Whether the integration is active, and its host/port/token, are runtime
 * settings (configurable from the web UI, persisted in flash) rather than a
 * compile-time flag, so this header no longer branches on a macro.
 */

#ifndef HOME_ASSISTANT_TRIGGER_H
#define HOME_ASSISTANT_TRIGGER_H

#include <Arduino.h>

/**
 * Initialize the Home Assistant connection settings
 *
 * Sets up the configuration needed to communicate with a Home Assistant
 * instance via its REST API. This must be called before any trigger events.
 *
 * @param host           Hostname or IP address of the Home Assistant instance
 * @param port           Port number (typically 8123 for Home Assistant)
 * @param apiKey         Long-lived access token for API authentication
 * @param scriptEndpoint API endpoint path for triggering automation scripts
 */
void initHomeAssistant(const char *host, uint16_t port, const char *apiKey, const char *scriptEndpoint);

/**
 * Trigger a Home Assistant automation when a number is selected
 *
 * Sends an HTTP POST request to the configured Home Assistant instance
 * with the selected dice number, which can trigger automations like
 * lighting effects corresponding to different game events. Does nothing
 * if the integration is currently disabled.
 *
 * @param selectedNumber The dice number that was selected (2-12, or 7 for robber)
 */
void triggerHomeAssistantScript(int selectedNumber);

/** Enables or disables sending triggers, without changing host/port/token. */
void setHomeAssistantEnabled(bool enabled);
bool isHomeAssistantEnabled();

String getHomeAssistantHost();
uint16_t getHomeAssistantPort();
String getHomeAssistantToken();

/**
 * Loads the persisted Home Assistant config from flash and applies it,
 * falling back to the given defaults the first time (before anything has
 * been saved via the web UI).
 */
void loadHomeAssistantConfig(const char *defaultHost, uint16_t defaultPort, const char *defaultToken, bool defaultEnabled);

/**
 * Applies new Home Assistant settings immediately and persists them to
 * flash so they survive a reboot.
 */
void configureHomeAssistant(const String &host, uint16_t port, const String &token, bool enabled);

#endif
