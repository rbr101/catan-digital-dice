/**
 * Catan Board Generator for ESP32
 *
 * This project provides a web interface for generating and displaying
 * randomized Catan boards with configurable placement rules. It includes
 * LED visualization using WS2812B LED strips, configurable game settings,
 * and support for both classic and extension Catan board layouts.
 *
 * Hardware Requirements:
 * - ESP32 development board
 * - WS2812B addressable LED strip (19 LEDs for classic mode, 30 for extension)
 * - Power supply for LEDs
 *
 * Features:
 * - Web-based interface accessible via WiFi
 * - Configurable board generation rules
 * - Physical LED board visualization
 * - Game state persistence through power cycles
 * - Support for classic (19 hexes) and extension (30 hexes) boards
 * - Home Assistant integration
 *
 * Adapted from the standalone smart-catan project to run alongside the
 * digital dice firmware on the esp32dev board; only compiled in when
 * SMART_CATAN is defined (see platformio.ini's [env:esp32dev]).
 */
#ifdef SMART_CATAN

// External Libraries
#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// Internal Project Headers
#include "BoardGenerator.h"
#include "WebPage.h"
#include "LedController.h"
#include "HomeAssistantTrigger.h"
#include "smart_catan.h"

// Uncomment to enable Home Assistant integration
// #define ENABLE_HOME_ASSISTANT

// Configuration for background task handling
#define BOARD_GEN_STACK_SIZE 8192 // Stack size for board generation task
#define BOARD_GEN_TASK_PRIORITY 1 // Priority level for the task
#define BOARD_GEN_TASK_CORE 1     // Core to run the task on (ESP32 has 2 cores)

// Global State Variables
volatile bool boardReady = true; // Indicates if board generation is complete
bool gameLoaded = false;         // Indicates if a saved game was loaded

// WiFi Credentials
// Note: These are placeholders. Create a password.h if you want to help in the development.
// ------------- WIFI & Home Assistant Credentials -------------
// Try to include password.h if it exists, otherwise use defaults
#if __has_include("password.h")
#include "password.h"
#else
                         // Default WiFi credentials - used when password.h doesn't exist
#ifndef WIFI_SSID
const char *WIFI_SSID = "PlaceholderSSID";
#endif
#ifndef WIFI_PASS
const char *WIFI_PASS = "PlaceholderPassword";
#endif

// Default Home Assistant credentials - only used if ENABLE_HOME_ASSISTANT is defined
#ifndef HA_IP
const char *HA_IP = "homeassistant.local";
#endif
#ifndef HA_PORT
const uint16_t HA_PORT = 8123;
#endif
#ifndef HA_ACCESS_TOKEN
const char *HA_ACCESS_TOKEN = "your_long_lived_access_token";
#endif
#endif

// Hardware Configuration
#ifndef LED_STRIP_PIN
#define LED_STRIP_PIN 4 // GPIO pin connected to the WS2812B data line; overridden by -DLED_STRIP_PIN in platformio.ini
#endif
#define LED_COUNT_CLASSIC 19   // Number of LEDs for classic board
#define LED_COUNT_EXTENSION 30 // Number of LEDs for extension board

// Initialize LED controller
LedController ledController(LED_STRIP_PIN, LED_COUNT_CLASSIC);

// Per-resource LED colors (index = resource ID, same encoding as the web
// UI: 0 sheep, 1 wood, 2 wheat, 3 brick, 4 ore, 5 desert), configurable from
// the web UI and persisted in flash. Populated by loadLedConfig() in setup().
uint32_t resourceLedColors[6];
static Preferences ledPrefs;
static const char *LED_NAMESPACE = "ledconfig";

// Currently selected dice number (2-12, 0 means none selected)
int selectedNumber;

// Default Configuration Settings
#define DEFAULT_EIGHT_SIX_CANTOUCH true    // Can 6 & 8 tokens be adjacent?
#define DEFAULT_TWO_TWELVE_CANTOUCH true   // Can 2 & 12 tokens be adjacent?
#define DEFAULT_SAMENUMBERS_CANTOUCH true  // Can identical token numbers be adjacent?
#define DEFAULT_SAMERESOURCE_CANTOUCH true // Can identical resources be adjacent?
#define DEFAULT_MANUAL_DICE false          // Allow manual dice number selection?
#define DEFAULT_IS_EXTENSION false         // Start in extension mode?

// Game state variables
bool manualDice;  // Manual dice selection enabled?
bool gameStarted; // Is game currently active?

// Web Server Setup
WebServer server(80); // HTTP server on port 80
String htmlPage;      // Stores the HTML content

// Catan Game Data
Board board;             // Current board layout
BoardConfig boardConfig; // Board configuration settings

//---------------------------------------------------------------
//                 UTILITY FUNCTIONS
//---------------------------------------------------------------

/**
 * Creates a JSON representation of the current game state
 *
 * @return String containing JSON data with board configuration,
 *         resource placement, number tokens, and game settings
 */
String generateJSON()
{
  // Create a JSON document
  JsonDocument doc;
  int ledNumber;

  // Determine number of hexes/LEDs based on board mode
  if (!boardConfig.isExtension)
  {
    ledNumber = LED_COUNT_CLASSIC;
  }
  else
  {
    ledNumber = LED_COUNT_EXTENSION;
  }

  // Add the resources array
  JsonArray resources = doc["resources"].to<JsonArray>();
  for (int i = 0; i < ledNumber; i++)
  {
    resources.add(board.resources[i]);
  }

  // Add the numbers array
  JsonArray numbers = doc["numbers"].to<JsonArray>();
  for (int i = 0; i < ledNumber; i++)
  {
    numbers.add(board.numbers[i]);
  }

  // Include the game mode and state flags
  doc["extension"] = boardConfig.isExtension;
  doc["gameStarted"] = gameStarted;

  // Include the game settings
  doc["eightSixCanTouch"] = boardConfig.eightSixCanTouch;
  doc["twoTwelveCanTouch"] = boardConfig.twoTwelveCanTouch;
  doc["sameNumbersCanTouch"] = boardConfig.sameNumbersCanTouch;
  doc["sameResourceCanTouch"] = boardConfig.sameResourceCanTouch;
  doc["manualDice"] = manualDice;

  // Include currently selected number
  doc["selectedNumber"] = selectedNumber;

  // Serialize the JSON document to a string
  String jsonResponse;
  serializeJson(doc, jsonResponse);

  return jsonResponse;
}

/**
 * Saves the current game state to the ESP32's flash memory
 * This allows persisting the game through power cycles
 */
void saveGameState()
{
  // Generate the json data
  String jsonString = generateJSON();

  // Write to SPIFFS (SPI Flash File System)
  File file = SPIFFS.open("/gamestate.json", FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.print(jsonString);
  file.close();
  Serial.println("Game state saved to flash.");
}

/**
 * Deletes any saved game state from flash memory
 */
void deleteGameState()
{
  if (SPIFFS.exists("/gamestate.json"))
  {
    SPIFFS.remove("/gamestate.json");
    Serial.println("Game state deleted from flash.");
  }
}

/**
 * Loads a previously saved game state from flash memory
 */
void loadGameState()
{
  Serial.print("Load Start!");
  if (SPIFFS.exists("/gamestate.json"))
  {
    Serial.print("Gamestate.json exists");
    File file = SPIFFS.open("/gamestate.json", FILE_READ);
    if (!file)
    {
      Serial.println("Failed to open game state file for reading");
      return;
    }
    String jsonString = file.readString();
    Serial.print("JSON STRING FROM FILE: ");
    Serial.println(jsonString);
    file.close();

    // Parse the JSON document
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
      Serial.print("Failed to parse game state: ");
      Serial.println(error.f_str());
      return;
    }

    // Load board configuration
    boardConfig.isExtension = doc["extension"];
    boardConfig.eightSixCanTouch = doc["eightSixCanTouch"];
    boardConfig.twoTwelveCanTouch = doc["twoTwelveCanTouch"];
    boardConfig.sameNumbersCanTouch = doc["sameNumbersCanTouch"];
    boardConfig.sameResourceCanTouch = doc["sameResourceCanTouch"];
    manualDice = doc["manualDice"];

    gameStarted = doc["gameStarted"];
    selectedNumber = doc["selectedNumber"];

    // Load board resources and numbers
    board.resources.clear();
    board.numbers.clear();

    JsonArray resources = doc["resources"].as<JsonArray>();
    for (JsonVariant v : resources)
    {
      board.resources.push_back(v.as<int>());
    }

    JsonArray numbers = doc["numbers"].as<JsonArray>();
    for (JsonVariant v : numbers)
    {
      board.numbers.push_back(v.as<int>());
    }
    Serial.println("Game state loaded from flash.");
  }
  else
  {
    Serial.println("No saved game state found in flash!");
  }
}

// --------------------------------------------------------------
//                  SERVER HANDLER FUNCTIONS
// --------------------------------------------------------------

// Defined further below; lights every tile with its resource color and
// leaves them statically on (used to preview the board before a game
// starts). Forward-declared here so boardGenerationTask can call it.
void showBoardResourceColors();

/**
 * FreeRTOS task that handles board generation in a separate thread
 * This prevents blocking the main loop during board calculation
 */
void boardGenerationTask(void *pvParameters)
{
  Serial.println("Board generation task started.");

  // Use the current boardConfig to generate a board
  board = generateBoard(boardConfig);

  // Signal that the board is ready
  Serial.println("Board generation complete.");
  boardReady = true;

  // Preview the freshly generated board on the LEDs. This task is only
  // (re)started when no game is in progress, so it's always safe to show
  // the idle board-preview colors here.
  showBoardResourceColors();

  // Delete the task when finished
  vTaskDelete(NULL);
}

/**
 * Creates a new task for board generation
 */
void createBoardTask()
{
  // Reset flag and create the board generation task
  if (boardReady)
  {
    boardReady = false;
    xTaskCreatePinnedToCore(
        boardGenerationTask, // Task function
        "BoardGenTask",      // Task name
        8192,                // Stack size (bytes)
        NULL,                // Parameters
        1,                   // Priority
        NULL,                // Task handle
        1                    // Run on core 1
    );

    // Wait until the board generation task is done
    while (!boardReady)
    {
      delay(10); // Yield to other tasks
    }
  }
}

/**
 * Web server handler for the root path
 * Serves the main HTML page
 */
void handleRoot()
{
  server.send(200, "text/html", htmlPage);
}

/**
 * Web server handler to update the "6 & 8 Can Touch" setting
 */
void handleUpdateEightSixCanTouch()
{
  String value = server.arg("value"); // "1" for true or "0" for false
  boardConfig.eightSixCanTouch = (value == "1");
  Serial.print("8 & 6 Can Touch set to: ");
  Serial.println(boardConfig.eightSixCanTouch ? "true" : "false");
  server.send(200, "text/plain", "eightSixCanTouch updated");
}

/**
 * Web server handler to update the "2 & 12 Can Touch" setting
 */
void handleUpdateTwoTwelveCanTouch()
{
  String value = server.arg("value");
  boardConfig.twoTwelveCanTouch = (value == "1");
  Serial.print("2 & 12 Can Touch set to: ");
  Serial.println(boardConfig.twoTwelveCanTouch ? "true" : "false");
  server.send(200, "text/plain", "twoTwelveCanTouch updated");
}

/**
 * Web server handler to update the "Same Numbers Can Touch" setting
 */
void handleUpdateSameNumbersCanTouch()
{
  String value = server.arg("value");
  boardConfig.sameNumbersCanTouch = (value == "1");
  Serial.print("Same Numbers Can Touch set to: ");
  Serial.println(boardConfig.sameNumbersCanTouch ? "true" : "false");
  server.send(200, "text/plain", "sameNumbersCanTouch updated");
}

/**
 * Web server handler to update the "Same Resource Can Touch" setting
 */
void handleUpdateSameResourceCanTouch()
{
  String value = server.arg("value");
  boardConfig.sameResourceCanTouch = (value == "1");
  Serial.print("Same Resource Can Touch set to: ");
  Serial.println(boardConfig.sameResourceCanTouch ? "true" : "false");
  server.send(200, "text/plain", "sameResourceCanTouch updated");
}

/**
 * Web server handler to update the "Manual Dice" setting
 */
void handleUpdateManualDice()
{
  String value = server.arg("value");
  manualDice = (value == "1");
  Serial.print("Manual Dice set to: ");
  Serial.println(manualDice ? "true" : "false");
  server.send(200, "text/plain", "manualDice updated");
}

/**
 * Web server handler to read the current Home Assistant configuration
 * Used by the settings page to prefill its form fields
 */
void handleGetHaConfig()
{
  JsonDocument doc;
  doc["enabled"] = isHomeAssistantEnabled();
  doc["host"] = getHomeAssistantHost();
  doc["port"] = getHomeAssistantPort();
  doc["token"] = getHomeAssistantToken();

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

/**
 * Web server handler to update the Home Assistant configuration
 * Applies the new settings immediately and persists them to flash
 */
void handleSetHaConfig()
{
  String host = server.arg("host");
  uint16_t port = (uint16_t)server.arg("port").toInt();
  String token = server.arg("token");
  bool enabled = server.arg("enabled") == "1";

  configureHomeAssistant(host, port, token, enabled);

  Serial.printf("[/sethaconfig] enabled=%d host=%s port=%u\n", enabled, host.c_str(), port);
  server.send(200, "text/plain", "Home Assistant config saved");
}

/**
 * Web server handler to set or shuffle classic board mode
 */
void handleSetClassic()
{
  Serial.println("[/setclassic] Request received. Setting game as classic");

  // Only reinitialize LED strip if it was in Extension mode before
  if (boardConfig.isExtension)
  {
    boardConfig.isExtension = false;
    ledController.restart(LED_COUNT_CLASSIC);
  }

  // Generate a new board
  createBoardTask();

  // Send the JSON response
  String jsonResponse = generateJSON();
  server.send(200, "application/json", jsonResponse);

  // Debug output
  Serial.println(jsonResponse);
}

/**
 * Web server handler to set or shuffle extension board mode
 */
void handleSetExtension()
{
  Serial.println("[/setextension] Request received. Setting game as Extension");

  // Only reinitialize LED strip if it was in Classic mode before
  if (!boardConfig.isExtension)
  {
    boardConfig.isExtension = true;
    ledController.restart(LED_COUNT_EXTENSION);
  }

  // Generate a new board
  createBoardTask();

  // Send the JSON response
  String jsonResponse = generateJSON();
  server.send(200, "application/json", jsonResponse);

  // Debug output
  Serial.println(jsonResponse);
}

/**
 * Web server handler to get current board state
 */
void handleGetBoard()
{
  Serial.println("[/getboard] Request received. Returning current board state.");
  // Only respond if the board has been initialized
  if (gameLoaded)
  {
    int totalHexes = boardConfig.isExtension ? LED_COUNT_EXTENSION : LED_COUNT_CLASSIC;

    // Generate the json data
    String jsonResponse = generateJSON();

    // Send the JSON response
    server.send(200, "application/json", jsonResponse);

    // Debug output
    Serial.println(jsonResponse);
  }
}

// --------------------------------------------------------------
//              SAVED BOARD LIBRARY (named board snapshots)
// --------------------------------------------------------------

const char *BOARD_FILE_PREFIX = "/board_";
const char *BOARD_FILE_SUFFIX = ".json";

/**
 * Turns a user-provided board name into a safe SPIFFS filename: keeps only
 * alphanumeric characters (replacing everything else with '_') and caps the
 * length so it fits SPIFFS's filename limit. The original name is stored
 * inside the file itself, so this only needs to be unique-ish, not pretty.
 */
String boardNameToFilename(const String &name)
{
  String safe;
  for (size_t i = 0; i < name.length() && safe.length() < 20; i++)
  {
    char c = name[i];
    safe += isalnum((unsigned char)c) ? c : '_';
  }
  if (safe.length() == 0)
  {
    safe = "board";
  }
  return String(BOARD_FILE_PREFIX) + safe + String(BOARD_FILE_SUFFIX);
}

/**
 * Web server handler to list all saved boards by name
 */
void handleListBoards()
{
  JsonDocument doc;
  JsonArray boards = doc.to<JsonArray>();

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file)
  {
    String path = String(file.name());
    if (!path.startsWith("/"))
    {
      path = "/" + path;
    }
    if (path.startsWith(BOARD_FILE_PREFIX) && path.endsWith(BOARD_FILE_SUFFIX))
    {
      File boardFile = SPIFFS.open(path, FILE_READ);
      if (boardFile)
      {
        JsonDocument boardDoc;
        if (deserializeJson(boardDoc, boardFile) == DeserializationError::Ok)
        {
          boards.add(boardDoc["name"].as<String>());
        }
        boardFile.close();
      }
    }
    file = root.openNextFile();
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

/**
 * Web server handler to save the current board under a given name
 */
void handleSaveBoard()
{
  String name = server.arg("name");
  if (name.length() == 0)
  {
    server.send(400, "text/plain", "Missing board name");
    return;
  }

  JsonDocument doc;
  doc["name"] = name;
  doc["isExtension"] = boardConfig.isExtension;
  JsonArray resources = doc["resources"].to<JsonArray>();
  for (int v : board.resources)
  {
    resources.add(v);
  }
  JsonArray numbers = doc["numbers"].to<JsonArray>();
  for (int v : board.numbers)
  {
    numbers.add(v);
  }

  File file = SPIFFS.open(boardNameToFilename(name), FILE_WRITE);
  if (!file)
  {
    server.send(500, "text/plain", "Failed to save board");
    return;
  }
  serializeJson(doc, file);
  file.close();

  Serial.printf("[/saveboard] Saved board '%s'\n", name.c_str());
  server.send(200, "text/plain", "Board saved");
}

/**
 * Web server handler to load a previously saved board by name, replacing
 * the current board. Refused while a game is in progress.
 */
void handleLoadBoard()
{
  if (gameStarted)
  {
    server.send(409, "text/plain", "Cannot load a board while a game is in progress");
    return;
  }

  String name = server.arg("name");
  String filename = boardNameToFilename(name);
  if (!SPIFFS.exists(filename))
  {
    server.send(404, "text/plain", "Board not found");
    return;
  }

  File file = SPIFFS.open(filename, FILE_READ);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error)
  {
    server.send(500, "text/plain", "Failed to parse saved board");
    return;
  }

  bool wantsExtension = doc["isExtension"];
  if (wantsExtension != boardConfig.isExtension)
  {
    boardConfig.isExtension = wantsExtension;
    ledController.restart(wantsExtension ? LED_COUNT_EXTENSION : LED_COUNT_CLASSIC);
  }

  board.resources.clear();
  board.numbers.clear();
  for (JsonVariant v : doc["resources"].as<JsonArray>())
  {
    board.resources.push_back(v.as<int>());
  }
  for (JsonVariant v : doc["numbers"].as<JsonArray>())
  {
    board.numbers.push_back(v.as<int>());
  }
  selectedNumber = 0;

  // Persist as the current board too, so it's still loaded after a reboot
  saveGameState();
  showBoardResourceColors();

  String jsonResponse = generateJSON();
  server.send(200, "application/json", jsonResponse);

  Serial.printf("[/loadboard] Loaded board '%s'\n", name.c_str());
}

/**
 * Web server handler to delete a previously saved board by name
 */
void handleDeleteBoard()
{
  String name = server.arg("name");
  SPIFFS.remove(boardNameToFilename(name));
  server.send(200, "text/plain", "Board deleted");
}

/**
 * Web server handler to get currently selected number
 */
void handleGetNumber()
{
  Serial.println("[/getnumber] Request received. Returning current selected number.");
  server.send(200, "application/json", String(selectedNumber));
}

/**
 * Web server handler to start a new game
 * Locks the board configuration and begins gameplay
 */
void handleStartGame()
{
  Serial.println("[/startgame] Request received. Starting game.");
  gameStarted = true;

  // Run start game animation on LEDs
  ledController.stopAnimation();
  ledController.startAnimation(START_GAME_ANIMATION, nullptr, 0, 250);

  // Generate JSON response and send it immediately; the flash write below
  // doesn't need to make the browser wait.
  String jsonResponse = generateJSON();
  server.send(200, "application/json", jsonResponse);

  // Save game state to flash for persistence
  saveGameState();

  // Debug output
  Serial.println(jsonResponse);
}

/**
 * Web server handler to end the current game
 * Unlocks board configuration
 */
void handleEndGame()
{
  Serial.println("[/endgame] Request received. Ending game.");
  gameStarted = false;
  selectedNumber = 0;

  // Back to idle: statically preview the board's resource colors again
  showBoardResourceColors();

  // Generate JSON response and send it immediately; deleting the saved
  // state below doesn't need to make the browser wait.
  String jsonResponse = generateJSON();
  server.send(200, "application/json", jsonResponse);

  // Delete the saved game state
  deleteGameState();

  // Debug output
  Serial.println(jsonResponse);
}

/**
 * Maps a resource ID to its configured LED color.
 *
 * @param resourceId Resource ID (0-5), same encoding as the web UI
 */
uint32_t getResourceLedColor(int resourceId)
{
  if (resourceId < 0 || resourceId > 5)
  {
    return ledController.Color(255, 255, 255);
  }
  return resourceLedColors[resourceId];
}

/**
 * Converts a packed RGB color to a "#rrggbb" hex string for the web UI's
 * color picker inputs.
 */
String colorToHex(uint32_t color)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)(color & 0xFFFFFF));
  return String(buf);
}

/**
 * Parses a "#rrggbb" (or "rrggbb") hex color string from the web UI into a
 * packed RGB color. Returns 0 (off) for anything that doesn't parse.
 */
uint32_t hexToColor(const String &hex)
{
  String h = hex;
  if (h.startsWith("#"))
  {
    h = h.substring(1);
  }
  if (h.length() != 6)
  {
    return 0;
  }
  return (uint32_t)strtoul(h.c_str(), nullptr, 16);
}

/**
 * Loads the LED brightness and per-resource colors from flash, falling back
 * to sensible defaults the first time (before anything has been saved via
 * the web UI), and applies them.
 */
void loadLedConfig()
{
  ledPrefs.begin(LED_NAMESPACE, true); // read-only
  uint8_t brightness = ledPrefs.getUChar("brightness", 80);
  resourceLedColors[0] = ledPrefs.getUInt("c0", ledController.Color(200, 255, 200)); // Sheep
  resourceLedColors[1] = ledPrefs.getUInt("c1", ledController.Color(0, 160, 0));     // Wood
  resourceLedColors[2] = ledPrefs.getUInt("c2", ledController.Color(255, 215, 0));   // Wheat
  resourceLedColors[3] = ledPrefs.getUInt("c3", ledController.Color(178, 34, 34));   // Brick
  resourceLedColors[4] = ledPrefs.getUInt("c4", ledController.Color(255, 255, 255)); // Ore
  resourceLedColors[5] = ledPrefs.getUInt("c5", 0);                                  // Desert - off
  ledPrefs.end();

  ledController.setBrightness(brightness);
}

/**
 * Applies new LED brightness/colors immediately and persists them to flash
 * so they survive a reboot.
 */
void saveLedConfig(uint8_t brightness, uint32_t colors[6])
{
  ledPrefs.begin(LED_NAMESPACE, false); // read-write
  ledPrefs.putUChar("brightness", brightness);
  for (int i = 0; i < 6; i++)
  {
    ledPrefs.putUInt(("c" + String(i)).c_str(), colors[i]);
    resourceLedColors[i] = colors[i];
  }
  ledPrefs.end();

  ledController.setBrightness(brightness);
}

/**
 * Web server handler to read the current LED brightness/color configuration
 * Used by the settings page to prefill the slider and color pickers
 */
void handleGetLedConfig()
{
  JsonDocument doc;
  doc["brightness"] = ledController.getBrightness();
  doc["sheep"] = colorToHex(resourceLedColors[0]);
  doc["wood"] = colorToHex(resourceLedColors[1]);
  doc["wheat"] = colorToHex(resourceLedColors[2]);
  doc["brick"] = colorToHex(resourceLedColors[3]);
  doc["ore"] = colorToHex(resourceLedColors[4]);
  doc["desert"] = colorToHex(resourceLedColors[5]);

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

/**
 * Web server handler to update the LED brightness/color configuration
 * Applies the new settings immediately and persists them to flash
 */
void handleSetLedConfig()
{
  uint8_t brightness = (uint8_t)server.arg("brightness").toInt();
  uint32_t colors[6] = {
      hexToColor(server.arg("sheep")),
      hexToColor(server.arg("wood")),
      hexToColor(server.arg("wheat")),
      hexToColor(server.arg("brick")),
      hexToColor(server.arg("ore")),
      hexToColor(server.arg("desert"))};

  saveLedConfig(brightness, colors);

  Serial.printf("[/setledconfig] brightness=%u\n", brightness);
  server.send(200, "text/plain", "LED config saved");

  // Repaint immediately with the new colors/brightness if no game is
  // running; turnOnNumber() will pick them up on its own during gameplay.
  if (!gameStarted)
  {
    showBoardResourceColors();
  }
}

/**
 * Lights every tile on the board with its resource color (matching the web
 * UI) and leaves them statically on - no animation, no blinking. Used to
 * preview the board on the LEDs before a game has started.
 */
void showBoardResourceColors()
{
  ledController.stopAnimation();

  int tileCount = boardConfig.isExtension ? LED_COUNT_EXTENSION : LED_COUNT_CLASSIC;
  for (int tile = 0; tile < tileCount; tile++)
  {
    ledController.turnTileOn(tile, getResourceLedColor(board.resources[tile]));
  }
  ledController.update();
}

/**
 * Updates the LED display based on the currently selected number
 * For normal numbers (2-6, 8-12): Lights up hexes with that number, using
 * that hex's resource color (matching the web UI)
 * For 7 (robber): Triggers the robber animation
 */
void turnOnNumber()
{
  // Determine number of tiles based on board mode
  int tileCount = boardConfig.isExtension ? LED_COUNT_EXTENSION : LED_COUNT_CLASSIC;
  ledController.stopAnimation();

  // Trigger Home Assistant with the selected number (no-op if disabled)
  triggerHomeAssistantScript(selectedNumber);

  // Turn off all LEDs before applying new state
  ledController.turnOffAllLeds();

  if (selectedNumber == 7)
  {
    // Special handling for the robber (7)
    uint8_t requiredTiles = boardConfig.isExtension ? 2 : 1;
    uint8_t foundCount = 0;

    // Allocate array for desert tiles (robber locations)
    uint16_t *robberTiles = new uint16_t[requiredTiles];

    // Find desert tiles (number token 0)
    for (int tile = 0; tile < tileCount && foundCount < requiredTiles; tile++)
    {
      if (board.numbers[tile] == 0)
      {
        Serial.print("Desert found at tile: ");
        Serial.println(tile);
        robberTiles[foundCount++] = tile;
      }
    }

    // Start robber animation from the desert tiles
    ledController.startAnimation(ROBBER_ANIMATION, robberTiles, foundCount, 500);
  }
  else
  {
    // For regular numbers, highlight matching hexes
    for (int tile = 0; tile < tileCount; tile++)
    {
      if (board.numbers[tile] == selectedNumber)
      {
        ledController.turnTileOn(tile, getResourceLedColor(board.resources[tile]));
      }
      else
      {
        // Turn off LEDs for non-matching tiles
        ledController.turnTileOn(tile, 0);
      }
    }

    // Update the LED strip
    ledController.update();
  }
}

/**
 * Web server handler to select a number during gameplay
 * Updates the selected number and highlights corresponding tiles
 */
void handleSelectNumber()
{
  // Get the number sent from the client
  String value = server.arg("value");
  selectedNumber = value.toInt();
  Serial.print("[/selectNumber] Number selected: ");
  Serial.println(selectedNumber);

  // Respond immediately so the web UI feels responsive; the LED update,
  // Home Assistant trigger, and flash save can happen after.
  server.send(200, "text/plain", value);

  // Update LEDs to reflect the selected number
  turnOnNumber();

  // Save the current game state
  saveGameState();
}

/**
 * Web server handler to simulate rolling dice
 * Generates random dice values and updates the display
 */
void handleRollDice()
{
  // Roll two dice (each die: 1 to 6)
  int die1 = random(1, 7);
  int die2 = random(1, 7);
  selectedNumber = die1 + die2;

  // Convert the total to a string
  String result = String(selectedNumber);

  // Respond immediately so the web UI feels responsive; the LED animation
  // (~1-1.5s) and flash save below would otherwise make the browser wait.
  server.send(200, "text/plain", result);

  // Run a dice roll animation on the LEDs
  ledController.rollDiceAnimation();

  // Turn off all LEDs (animation ends with all on)
  ledController.turnOffAllLeds();

  // Update the board display
  turnOnNumber();

  // Save the current game state
  saveGameState();
}

// --------------------------------------------------------------
//                  SETUP & LOOP
// --------------------------------------------------------------

/**
 * Called from the dice firmware's setup(), after Serial.begin() and the
 * display/dice init, when SMART_CATAN is defined.
 * Initializes hardware, loads configuration, and starts the web server.
 */
void smartCatanSetup()
{
  // Initialize the SPI Flash File System
  if (!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Attempt to load saved game state
  loadGameState();

  // If no game state was loaded, set default configuration
  if (board.resources.size() == 0)
  {
    Serial.print("No settings in flash! Loading defaults");
    boardConfig.isExtension = DEFAULT_IS_EXTENSION;
    boardConfig.eightSixCanTouch = DEFAULT_EIGHT_SIX_CANTOUCH;
    boardConfig.twoTwelveCanTouch = DEFAULT_TWO_TWELVE_CANTOUCH;
    boardConfig.sameNumbersCanTouch = DEFAULT_SAMENUMBERS_CANTOUCH;
    boardConfig.sameResourceCanTouch = DEFAULT_SAMERESOURCE_CANTOUCH;
    manualDice = DEFAULT_MANUAL_DICE;
    gameStarted = false;
    selectedNumber = 0;
  }

  // Connect to WiFi
  connectWifi(WIFI_SSID, WIFI_PASS);

  // Load HTML content from SPIFFS
  readHtml(htmlPage, server);

  // Seed the random number generator
  randomSeed(micros());

  // Initialize LED strip based on board mode
  uint16_t ledCount = boardConfig.isExtension ? LED_COUNT_EXTENSION : LED_COUNT_CLASSIC;
  ledController.begin(ledCount);

  // Load brightness/per-resource colors from flash (or defaults on first
  // boot) now that the strip exists
  loadLedConfig();

  // Start appropriate LED display
  if (board.resources.size() == 0)
  {
    // No board yet: show the waiting animation while one is generated
    // below. boardGenerationTask() switches to the static resource-color
    // preview as soon as it's done.
    ledController.startAnimation(WAITING_ANIMATION, nullptr, 0, 50);
  }
  else if (gameStarted)
  {
    // A game was already in progress when we rebooted: restore the
    // selected-number highlight instead of the idle board preview.
    turnOnNumber();
  }
  else
  {
    // Board loaded but no game running: statically preview it on the LEDs
    showBoardResourceColors();
  }

  // Set up server routes
  server.on("/", HTTP_GET, handleRoot);

  // Settings endpoints
  server.on("/eightSixCanTouch", HTTP_GET, handleUpdateEightSixCanTouch);
  server.on("/twoTwelveCanTouch", HTTP_GET, handleUpdateTwoTwelveCanTouch);
  server.on("/sameNumbersCanTouch", HTTP_GET, handleUpdateSameNumbersCanTouch);
  server.on("/sameResourceCanTouch", HTTP_GET, handleUpdateSameResourceCanTouch);
  server.on("/manualDice", HTTP_GET, handleUpdateManualDice);
  server.on("/gethaconfig", HTTP_GET, handleGetHaConfig);
  server.on("/sethaconfig", HTTP_GET, handleSetHaConfig);
  server.on("/getledconfig", HTTP_GET, handleGetLedConfig);
  server.on("/setledconfig", HTTP_GET, handleSetLedConfig);

  // Game control endpoints
  server.on("/setclassic", HTTP_GET, handleSetClassic);
  server.on("/setextension", HTTP_GET, handleSetExtension);
  server.on("/getboard", HTTP_GET, handleGetBoard);
  server.on("/getnumber", HTTP_GET, handleGetNumber);
  server.on("/listboards", HTTP_GET, handleListBoards);
  server.on("/saveboard", HTTP_GET, handleSaveBoard);
  server.on("/loadboard", HTTP_GET, handleLoadBoard);
  server.on("/deleteboard", HTTP_GET, handleDeleteBoard);
  server.on("/startgame", HTTP_GET, handleStartGame);
  server.on("/endgame", HTTP_GET, handleEndGame);
  server.on("/selectNumber", HTTP_GET, handleSelectNumber);
  server.on("/rollDice", HTTP_GET, handleRollDice);

  // Generate a new board if none was loaded
  if (board.resources.size() == 0)
  {
    Serial.print("No board loaded, generating new board!");
    xTaskCreatePinnedToCore(
        boardGenerationTask,     // Task function
        "BoardGenTask",          // Task name
        BOARD_GEN_STACK_SIZE,    // Stack size
        NULL,                    // Parameters
        BOARD_GEN_TASK_PRIORITY, // Priority
        NULL,                    // Task handle
        BOARD_GEN_TASK_CORE      // Core to run on
    );
  }
  else
  {
    Serial.println("Using saved board state.");
  }

  // Load Home Assistant config from flash (host/port/token/enabled), so
  // settings saved earlier via the web UI survive this reboot. The
  // ENABLE_HOME_ASSISTANT macro from password.h only supplies the very
  // first boot's default, before anything has been saved.
#ifdef ENABLE_HOME_ASSISTANT
  loadHomeAssistantConfig(HA_IP, HA_PORT, HA_ACCESS_TOKEN, true);
#else
  loadHomeAssistantConfig(HA_IP, HA_PORT, HA_ACCESS_TOKEN, false);
#endif
  Serial.printf("Home Assistant integration: %s (host=%s port=%u)\n",
                isHomeAssistantEnabled() ? "enabled" : "disabled",
                getHomeAssistantHost().c_str(), getHomeAssistantPort());

   // Initialize mDNS
  if (!MDNS.begin("smartcatan")) {   // Set the hostname to "smartcatan.local"
    Serial.println("Error setting up MDNS responder!");
    while(1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");

  // Start the web server
  server.begin();
  Serial.println("HTTP server started.");

  // Mark initialization as complete
  gameLoaded = true;
}

/**
 * Called from the dice firmware's loop() on every iteration, when
 * SMART_CATAN is defined. Handles web server client requests.
 */
void smartCatanLoop()
{
  server.handleClient();

  // Periodic WiFi status heartbeat, so the status is visible within a few
  // seconds of opening the serial monitor at any point, not just at boot.
  static unsigned long lastWifiLog = 0;
  if (millis() - lastWifiLog > 5000)
  {
    lastWifiLog = millis();
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.printf("[WiFi] connected | IP: %s | RSSI: %d dBm\n",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    else
    {
      Serial.printf("[WiFi] not connected | status code: %d | reconnecting...\n", WiFi.status());
      // Belt-and-suspenders: WiFi.setAutoReconnect(true) handles most drops,
      // but explicitly nudge it in case the AP was unreachable for a while.
      WiFi.reconnect();
    }
  }
}

/**
 * Called from the dice firmware's rollDice() (BASE variant only) right after
 * a physical roll, mirroring handleRollDice()'s body minus the HTTP response:
 * updates the LED board for the given total and (if enabled) triggers the
 * Home Assistant webhook, the same as pressing "Roll Dice" in the web UI.
 */
void notifySmartCatanRoll(int total)
{
  // Board generation/WiFi may still be in progress right after boot; skip
  // rather than touch an empty board.
  if (!gameLoaded || board.resources.size() == 0)
    return;

  selectedNumber = total;

  // Run a dice roll animation on the LEDs
  ledController.rollDiceAnimation();

  // Turn off all LEDs (animation ends with all on)
  ledController.turnOffAllLeds();

  // Update the board display
  turnOnNumber();

  // Save the current game state
  saveGameState();
}

#endif // SMART_CATAN
