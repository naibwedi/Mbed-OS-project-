/**
 * @file main.cpp
 * authors Naibe Mehari Tekle; Amin Martin Berge;
 *         Haben Goitom Gebreluul; Abdifatah Mohyadin Alasaow ;safwa ahmad kakeh
 */


#include "DFRobot_RGBLCD1602.h"
#include "app_ipgeolocation_io_certificate.h"
#include "HTS221Sensor.h"
#include "Kernel.h"
#include "TempHumiditySensor.h"
#include "json.hpp"
#include "mbed.h"
#include "UserInput.h"
#include "weather.h"
#include "newsfeed.h" 
#include "wifi.h"
#include "alarm.h"
#include "display.h"
#include "fetch.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <ostream>
#include <sstream>
#include <string>


#define WAIT_TIME_MS (5000ms)
#define DEBOUNCE_TIME_MS 50
#define LONG_PRESS_TIME_MS 500
#define NEWS_FEED_DISPLAY_TIME_MS 30000

DigitalOut rled(LED1); // LED for indicating activity
I2C dev(PB_9, PB_8); // I2C interface for the LCD
DFRobot_RGBLCD1602 lcd(&dev); // LCD display

using json = nlohmann::json;

InterruptIn hourButton(PD_14, PullUp); // Button for incrementing hour
InterruptIn minuteButton(PA_3, PullUp); // Button for incrementing minute
InterruptIn toggleAlarmButton(PA_0, PullUp); // Button for toggling alarm
InterruptIn snoozeButton(PB_4, PullUp); // Button for snoozing alarm
InterruptIn muteAlarmButton(PC_13, PullUp); // Button for muting alarm

enum Screen {
  DEFAULT_SCREEN,
  TEMP_HUMIDITY_SCREEN,
  WEATHER_SCREEN,
  USER_INPUT_SCREEN,
  NEWS_FEED_SCREEN
};

Screen currentScreen = DEFAULT_SCREEN; // Current screen being displayed

EventQueue eventQueue; // Event queue for handling events
Thread eventThread; // Thread for event queue
Timer toggleTimer; // Timer for button press duration
bool toggleButtonPressed = false; // Flag for button press state
Kernel::Clock::time_point lastScreenUpdate;
Kernel::Clock::time_point lastDateTimeUpdate;
Kernel::Clock::time_point newsFeedStartTime;

constexpr auto screenUpdateInterval = 15min; // Interval for updating screen
constexpr auto dateTimeUpdateInterval = 1min; // Interval for updating date/time

std::vector<std::string> newsHeadlines; // Vector for storing news headlines
std::string userLocationInput = "Oslo"; // Default user location input
bool userInputProcessed = false; // Flag for processing user input

void toggleButtonPressedHandler() {
    toggleTimer.reset();
    toggleTimer.start();
    toggleButtonPressed = true;
}

void toggleButtonReleasedHandler() {
    toggleTimer.stop();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(toggleTimer.elapsed_time());

    if (elapsed.count() > LONG_PRESS_TIME_MS) { // Long press
        printf("Switching screens: %llu ms\n", elapsed.count());
        currentScreen = static_cast<Screen>((currentScreen + 1) % 5);
        lcd.clear(); // Clear the LCD screen when switching

        if (currentScreen == NEWS_FEED_SCREEN) {
            newsFeedStartTime = Kernel::Clock::now(); // Record the start time of the news feed screen
        }
    } else if (elapsed.count() > DEBOUNCE_TIME_MS) { // Short press
        printf("Toggling alarm: %llu ms\n", elapsed.count());
        toggleAlarm();
    }
    toggleButtonPressed = false;
}

int main() {
    lcd.init(); // Initialize the LCD
    lcd.display(); // Turn on the LCD display

    initTempHumiditySensor(); // Initialize temperature and humidity sensor
    initUART(); // Initialize UART for user input

    NetworkInterface *network = nullptr;
    do {
        network = NetworkInterface::get_default_instance();
        if (!network) {
            printf("Failed to get default network interface\n");
        }
        ThisThread::sleep_for(1s);
    } while (network == nullptr);

    nsapi_size_or_error_t result;
    do {
        result = network->connect();
        if (result != NSAPI_ERROR_OK) {
            printf("Failed to connect to network: %d %s\n", result, get_nsapi_error_string(result));
        }
    } while (result != NSAPI_ERROR_OK);

    SocketAddress address;
    do {
        result = network->get_ip_address(&address);
        if (result != NSAPI_ERROR_OK) {
            printf("Failed to get local IP address: %d %s\n", result, get_nsapi_error_string(result));
        }
    } while (result != NSAPI_ERROR_OK);

    printf("Connected to WLAN and got IP address %s\n", address.get_ip_address());

    json document;
    fetchJSONData(network, document); // Fetch initial data from API
    setRTCAndDisplayInformation(document); // Set RTC and display information

    eventThread.start(callback(&eventQueue, &EventQueue::dispatch_forever)); // Start event thread

    fetchRSSFeed(network, newsHeadlines); // Fetch initial news headlines

    hourButton.rise(eventQueue.event(incrementHour)); // Bind hour button event
    minuteButton.rise(eventQueue.event(incrementMinute)); // Bind minute button event
    toggleAlarmButton.fall(eventQueue.event(toggleButtonPressedHandler)); // Bind toggle button press event
    toggleAlarmButton.rise(eventQueue.event(toggleButtonReleasedHandler)); // Bind toggle button release event
    snoozeButton.rise(eventQueue.event(snoozeAlarm)); // Bind snooze button event
    muteAlarmButton.rise(eventQueue.event(muteAlarm)); // Bind mute button event

    while (true) {
        rled = !rled; // Toggle activity LED
        auto now = Kernel::Clock::now();

        if (now - lastDateTimeUpdate >= dateTimeUpdateInterval) {
            fetchJSONData(network, document); // Fetch updated JSON data
            lastDateTimeUpdate = now;
        }

        if (currentScreen == NEWS_FEED_SCREEN &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - newsFeedStartTime).count() >= NEWS_FEED_DISPLAY_TIME_MS) {
            currentScreen = DEFAULT_SCREEN; // Switch back to default screen after news feed display time
        }

        switch (currentScreen) {
            case DEFAULT_SCREEN:
                updateDateTimeDisplay(document); // Update date and time display
                checkAndTriggerAlarm(document); // Check and trigger alarm
                updateAlarmDisplay(); // Update alarm display
                break;
            case TEMP_HUMIDITY_SCREEN:
                readAndDisplayTempHumidity(lcd); // Read and display temperature and humidity
                break;
            case WEATHER_SCREEN:
                if (!userLocationInput.empty()) {
                    char weatherApiUrl[512];
                    snprintf(weatherApiUrl, sizeof(weatherApiUrl), "http://api.weatherapi.com/v1/current.json?key=9a87ee985e58449ab78183815222505&q=%s", userLocationInput.c_str());
                    fetchWeather(network, lcd, weatherApiUrl); // Fetch and display weather information
                    userLocationInput.clear();
                    userInputProcessed = false;
                }
                break;
            case USER_INPUT_SCREEN:
                if (!userInputProcessed) {
                    processUserInput(lcd, userLocationInput); // Process user input for location
                    char weatherApiUrl[512];
                    snprintf(weatherApiUrl, sizeof(weatherApiUrl), "http://api.weatherapi.com/v1/current.json?key=9a87ee985e58449ab78183815222505&q=%s", userLocationInput.c_str());
                    fetchWeather(network, lcd, weatherApiUrl); // Fetch and display weather information
                    userInputProcessed = true;
                }
                break;
            case NEWS_FEED_SCREEN:
                fetchRSSFeed(network, newsHeadlines); // Fetch news headlines
                displayNewsHeadlines(lcd, newsHeadlines, "CNN"); // Display news headlines
                break;
            default:
                currentScreen = DEFAULT_SCREEN; // Default screen
                break;
        }

        // Debugging
        printf("Current screen: %d, Alarm enabled: %s, Alarm active: %s\n", currentScreen, alarmEnabled ? "Yes" : "No", alarmActive ? "Yes" : "No");
        printf("Remaining stack space = %u bytes\n", osThreadGetStackSpace(ThisThread::get_id()));
        ThisThread::sleep_for(WAIT_TIME_MS);
    }
}