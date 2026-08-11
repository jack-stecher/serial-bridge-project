#include <windows.h>
#include <iostream>
#include <string>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <conio.h>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <cctype>

struct SensorReading {
    int potentiometer;
    int buttonPressed;
    int lightLevel;
};

// The reader thread updates this state while the main thread handles all
// console drawing and keyboard input. Keeping console output in one thread
// prevents the two threads from fighting over the console cursor.
struct DashboardState {
    SensorReading sensor{0, 0, 0};
    std::string lastUpdate = "Waiting for data...";
    std::string logFile;
    std::string lastResponse = "Waiting for response...";
    std::string ledRed = "OFF";
    std::string ledGreen = "OFF";
    std::string ledBlue = "OFF";
    std::string servoAngle = "0";
    std::string currentInput;
};

std::mutex stateMutex;
std::atomic<bool> running{true};
std::atomic<bool> dashboardDirty{true};
DashboardState dashboard;

constexpr int LINE_WIDTH = 72;
const std::string DIVIDER(LINE_WIDTH, '-');
const std::string TITLE_BAR(LINE_WIDTH, '=');

// Formats the current local time as "YYYY-MM-DD HH:MM:SS.mmm" for CSV rows
// and the "Last Update" field.
std::string currentTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t); // Windows-safe alternative to localtime()

    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// parse example line DATA:x,x,x
// Returns std::nullopt if the line isn't a telemetry line at all (e.g. it's
// a command echo/response like "Servo set to 90"), or if a telemetry line is
// malformed, instead of crashing.
std::optional<SensorReading> parseLine(const std::string& line) {
    const std::string prefix = "DATA:";
    if (line.rfind(prefix, 0) != 0) return std::nullopt;

    std::string data = line.substr(prefix.size());
    SensorReading reading{};
    size_t comma1 = data.find(',');
    size_t comma2 = data.find(',', comma1 + 1);
    if (comma1 == std::string::npos || comma2 == std::string::npos) return std::nullopt;

    try {
        reading.potentiometer = std::stoi(data.substr(0, comma1));
        reading.buttonPressed = std::stoi(data.substr(comma1 + 1, comma2 - comma1 - 1));
        reading.lightLevel = std::stoi(data.substr(comma2 + 1));
    }
    catch (const std::exception&) {
        // stoi throws on empty/non-numeric text (invalid_argument) or a
        // number too large to fit in an int (out_of_range) -- either way,
        // treat it as a bad line rather than letting the program crash.
        return std::nullopt;
    }
    return reading;
}

// Updates the dashboard state from a command response. The main thread will
// display these values the next time the dashboard is redrawn.
void updateResponseState(const std::string& line) {
    dashboard.lastResponse = line;
    if (line.rfind("LED Red set to ", 0) == 0) {
        dashboard.ledRed = line.substr(15) == "1" ? "ON" : "OFF";
    }
    else if (line.rfind("LED Green set to ", 0) == 0) {
        dashboard.ledGreen = line.substr(17) == "1" ? "ON" : "OFF";
    }
    else if (line.rfind("LED Blue set to ", 0) == 0) {
        dashboard.ledBlue = line.substr(16) == "1" ? "ON" : "OFF";
    }
    else if (line.rfind("Servo set to ", 0) == 0) {
        dashboard.servoAngle = line.substr(13);
    }
}

// The main thread is the only thread that writes to the console. It builds
// the whole frame as one string and writes it in a single flush, moving the
// cursor back to the top-left rather than clearing the screen. Combined with
// the alternate screen buffer entered in main(), this redraws in place
// without pushing each frame into scrollback (which is what caused the
// dashboard to look like it was reprinting/scrolling endlessly).
void drawDashboard() {
    DashboardState state;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state = dashboard;
    }

    std::ostringstream out;
    out << "\x1b[H"; // cursor home -- \x1b[K on each line below clears leftover text instead

    out << TITLE_BAR << '\n';
    out << "                         Serial Bridge Dashboard\n";
    out << TITLE_BAR << "\n\n";

    out << " Potentiometer : " << state.sensor.potentiometer << " \x1b[K\n";
    out << " Button        : " << (state.sensor.buttonPressed ? "PRESSED" : "RELEASED") << " \x1b[K\n";
    out << " Light Level   : " << state.sensor.lightLevel << " \x1b[K\n";
    out << " Last Update   : " << state.lastUpdate << " \x1b[K\n";
    out << " Log File      : " << state.logFile << " \x1b[K\n\n";

    out << DIVIDER << '\n';
    out << " LED Red       : " << state.ledRed << " \x1b[K\n";
    out << " LED Green     : " << state.ledGreen << " \x1b[K\n";
    out << " LED Blue      : " << state.ledBlue << " \x1b[K\n";
    out << " Servo Angle   : " << state.servoAngle << " \x1b[K\n\n";

    out << DIVIDER << '\n';
    out << " Last Response : " << state.lastResponse << " \x1b[K\n\n";

    out << DIVIDER << '\n';
    out << " Commands: R1/R0  G1/G0  B1/B0  S000-S180  q (quit)\n\n";
    out << "> " << state.currentInput << " \x1b[K";

    std::cout << out.str();
    std::cout.flush();
}

// Sends a command string to the Arduino over the open serial handle.
// Appends '\n' so it lines up with readStringUntil('\n') on the Arduino side.
void sendCommand(HANDLE hSerial, const std::string& cmd) {
    std::string toSend = cmd + "\n";
    DWORD bytesWritten;
    if (!WriteFile(hSerial, toSend.c_str(), (DWORD)toSend.size(), &bytesWritten, nullptr)) {
        std::lock_guard<std::mutex> lock(stateMutex);
        dashboard.lastResponse = "ERROR: Failed to send command: " + cmd;
        dashboardDirty = true;
    }
}

// Runs on its own thread. Continuously reads bytes from the Arduino,
// assembles complete lines, updates the dashboard state, and logs
// each valid reading to the CSV file.
void readerLoop(HANDLE hSerial, std::ofstream& csv) {
    std::string buffer;
    char byte;
    DWORD bytesRead;

    while (running) {
        if (ReadFile(hSerial, &byte, 1, &bytesRead, nullptr)) {
            if (bytesRead > 0) {
                buffer += byte;
                if (byte == '\n') {
                    buffer.pop_back();
                    if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();

                    std::optional<SensorReading> reading = parseLine(buffer);
                    if (reading.has_value()) {
                        std::string timestamp = currentTimestamp();
                        {
                            std::lock_guard<std::mutex> lock(stateMutex);
                            dashboard.sensor = *reading;
                            dashboard.lastUpdate = timestamp;
                        }
                        dashboardDirty = true;

                        // Append the row and flush immediately so data
                        // survives even if the program is closed abruptly
                        // rather than sitting in a buffer.
                        csv << timestamp << "," << reading->potentiometer << ","
                            << reading->buttonPressed << "," << reading->lightLevel << "\n";
                        csv.flush();
                    }
                    else if (!buffer.empty()) {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        // Non-telemetry lines are command responses/echoes from the Arduino.
                        updateResponseState(buffer);
                        dashboardDirty = true;
                    }
                    buffer.clear();
                }
            }
            // bytesRead == 0 just means the 50ms read timeout elapsed with
            // no data -- expected while idle, so we loop back and check
            // `running` again rather than treating it as an error.
        }
        else {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                dashboard.lastResponse = "ERROR: Read error -- check the connection.";
            }
            dashboardDirty = true;
            running = false;
        }
    }
}

int main() {
    HANDLE hSerial = CreateFileA(
        "\\\\.\\COM3", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cout << "Failed to open COM3.\n";
        return 1;
    }

    // configure serial port
    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_9600;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);

    // setup read timeouts
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);

    // Make sure the console actually interprets ANSI escape sequences.
    // On by default in modern Windows Terminal/VS Code, but not guaranteed
    // in every host, so set it explicitly rather than assume.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    GetConsoleMode(hOut, &consoleMode);
    SetConsoleMode(hOut, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // Enter the alternate screen buffer -- the same mechanism used by
    // htop/vim/less to redraw a fixed region in place without polluting
    // scrollback. This is what actually fixes the "prints over and over"
    // scrolling behavior; clearing alone doesn't stop that.
    std::cout << "\x1b[?1049h";

    // Build a unique filename per run so successive sessions don't
    // overwrite each other's data.
    std::string csvFilename = "sensor_log_" + [] {
        std::string ts = currentTimestamp();
        for (char& c : ts) {
            if (c == ':' || c == ' ' || c == '.') c = '-';
        }
        return ts;
    }() + ".csv";

    std::ofstream csv(csvFilename);
    if (!csv.is_open()) {
        std::cout << "\x1b[?1049l"; // leave alt buffer before printing the error
        std::cout << "Failed to open CSV file for writing: " << csvFilename << "\n";
        CloseHandle(hSerial);
        return 1;
    }

    csv << "Timestamp,Potentiometer,Button,Light\n";
    csv.flush();

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        dashboard.logFile = csvFilename;
    }

    // First frame needs a real clear since the alt buffer may carry stale
    // content from a prior program's use of it.
    std::cout << "\x1b[2J";
    drawDashboard();

    // Start the reader on its own thread so it can keep receiving serial data
    // while the main thread handles keyboard input and console drawing.
    std::thread reader(readerLoop, hSerial, std::ref(csv));

    using clock = std::chrono::steady_clock;
    auto lastDraw = clock::now();

    // Cap redraws to ~20fps so a flood of incoming sensor lines can't
    // trigger dozens of redraws per second.
    constexpr auto minRedrawInterval = std::chrono::milliseconds(50);

    while (running) {
        auto now = clock::now();
        if (dashboardDirty && (now - lastDraw) >= minRedrawInterval) {
            dashboardDirty = false;
            drawDashboard();
            lastDraw = now;
        }

        if (_kbhit()) {
            char key = _getch();

            if (key == '\r') {
                std::string submitted;
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    submitted = dashboard.currentInput;
                    dashboard.currentInput.clear();
                }

                if (submitted == "q" || submitted == "Q") {
                    {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        dashboard.lastResponse = "Quitting...";
                    }
                    dashboardDirty = true;
                    running = false;
                    break;
                }

                if (!submitted.empty()) sendCommand(hSerial, submitted);
                dashboardDirty = true;
            }
            else if (key == '\b') {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (!dashboard.currentInput.empty()) dashboard.currentInput.pop_back();
                dashboardDirty = true;
            }
            else if (isprint(static_cast<unsigned char>(key))) {
                std::lock_guard<std::mutex> lock(stateMutex);
                dashboard.currentInput += key;
                dashboardDirty = true;
            }
        }

        // Avoid pegging a CPU core while idle.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for the reader thread to notice `running == false` and exit
    // cleanly before we close the handle out from under it.
    reader.join();

    csv.close();
    CloseHandle(hSerial);

    // Leave the alt screen buffer so the shell prompt returns normally
    // instead of staying on the dashboard's blank final frame.
    std::cout << "\x1b[?1049l";

    return 0;
}