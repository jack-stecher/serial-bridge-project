#include <windows.h>  //windows functions/types include
#include <iostream>
#include <string>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <conio.h>    // _kbhit() / _getch(), used for the exit key
#include <fstream>
#include <iomanip>
#include <ctime>
#include <sstream>


struct SensorReading {
    int potentiometer;
    int buttonPressed;
    int lightLevel;
};

// Guards std::cout AND the shared input buffer below, so the reader thread
// and main thread never write to the console at the same time and never
// see a half-updated command string.
std::mutex consoleMutex;

// Shared flag both threads check to know when to stop.
std::atomic<bool> running{true};

// What the user has typed so far on the current, not-yet-submitted command.
// Owned by main(), but read by the reader thread under consoleMutex so it
// can redraw it after printing a sensor line.
std::string currentInput;

// Redraws the "> <partial command>" prompt line. Caller must hold consoleMutex.
void redrawPromptLocked() {
    std::cout << "\r> " << currentInput << std::flush;
}

// Clears the current prompt line, prints `text` (which should end in a
// newline), then redraws whatever the user had typed so far. This is the
// single choke point both threads use to write to the console, so a
// sensor line printed mid-keystroke never splices into the typed text.
void printLine(const std::string& text) {
    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cout << "\r" << std::string(currentInput.size() + 2, ' ') << "\r";
    std::cout << text;
    redrawPromptLocked();
}

// Formats the current local time as "YYYY-MM-DD HH:MM:SS.mmm" for CSV rows.
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

// parse example line x,x,x
// Returns std::nullopt if the line is malformed instead of crashing,
// since a partial/garbled line from the Arduino is expected occasionally
// (e.g. right after the port opens, or during a reset).
std::optional<SensorReading> parseLine(const std::string& line) {
    SensorReading reading{};

    // Find the first/second commas
    size_t comma1 = line.find(',');
    size_t comma2 = line.find(',', comma1 + 1);

    // Make sure both commas were found
    if (comma1 == std::string::npos || comma2 == std::string::npos) {
        return std::nullopt;
    }

    // Extract and convert each value
    try {
        reading.potentiometer =
            std::stoi(line.substr(0, comma1));

        reading.buttonPressed =
            std::stoi(line.substr(comma1 + 1, comma2 - comma1 - 1));

        reading.lightLevel =
            std::stoi(line.substr(comma2 + 1));
    } catch (const std::exception&) {
        // stoi throws on empty/non-numeric text (invalid_argument) or a
        // number too large to fit in an int (out_of_range) -- either way,
        // treat it as a bad line rather than letting the program crash.
        return std::nullopt;
    }

    return reading;
}

// Sends a command string to the Arduino over the open serial handle.
// Appends '\n' so it lines up with readStringUntil('\n') on the Arduino side.
void sendCommand(HANDLE hSerial, const std::string& cmd) {
    std::string toSend = cmd + "\n";
    DWORD bytesWritten;
    if (!WriteFile(hSerial, toSend.c_str(), (DWORD)toSend.size(), &bytesWritten, nullptr)) {
        printLine("Failed to send command: " + cmd + "\n");
    }
}

// Runs on its own thread. Continuously reads bytes from the Arduino,
// assembles complete lines, prints parsed sensor readings, and logs each
// valid reading to the CSV file with a timestamp.
void readerLoop(HANDLE hSerial, std::ofstream& csv) {
    std::string buffer;
    char byte;
    DWORD bytesRead;

    while (running) {
        if (ReadFile(hSerial, &byte, 1, &bytesRead, nullptr)) {

            if (bytesRead > 0) {
                buffer += byte;

                if (byte == '\n') {
                    buffer.pop_back(); // remove '\n'

                    if (!buffer.empty() && buffer.back() == '\r') {
                        buffer.pop_back(); // remove '\r' if present
                    }

                    std::optional<SensorReading> reading = parseLine(buffer);

                    if (reading.has_value()) {
                        std::string timestamp = currentTimestamp();

                        std::string line =
                            "Potentiometer: " + std::to_string(reading->potentiometer) +
                            " | Button: " + std::to_string(reading->buttonPressed) +
                            " | Light: " + std::to_string(reading->lightLevel) + "\n";
                        printLine(line);

                        // Append the row and flush immediately so data
                        // survives even if the program is closed abruptly
                        // (e.g. Ctrl+C) rather than sitting in a buffer.
                        csv << timestamp << ","
                            << reading->potentiometer << ","
                            << reading->buttonPressed << ","
                            << reading->lightLevel << "\n";
                        csv.flush();
                    } else {
                        printLine("Skipped malformed line.\n");
                    }

                    buffer.clear();
                }
            }
            // bytesRead == 0 just means the 50ms read timeout elapsed with
            // no data -- expected while idle, so we loop back and check
            // `running` again rather than treating it as an error.
        } else {
            printLine("Read error -- check the connection.\n");
            running = false;
        }
    }
}

int main() {
    HANDLE hSerial = CreateFileA(      // handle to connect to COM3
        "\\\\.\\COM3",      // open device COM3
        GENERIC_READ | GENERIC_WRITE,       // to and from serial port
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cout << "Failed to open COM3.\n";
        return 1;
    }

    // configure serial port
    DCB dcbSerialParams = {0}; //devicecontrolblock
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_9600; // 9600bitsps
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);

    //setup read timeouts
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);

    // Build a unique filename per run so successive sessions don't
    // overwrite each other's data.
    std::string csvFilename = "sensor_log_" + [] {
        std::string ts = currentTimestamp();
        // Replace characters that aren't valid in Windows filenames.
        for (char& c : ts) {
            if (c == ':' || c == ' ' || c == '.') c = '-';
        }
        return ts;
    }() + ".csv";

    std::ofstream csv(csvFilename);
    if (!csv.is_open()) {
        std::cout << "Failed to open CSV file for writing: " << csvFilename << "\n";
        CloseHandle(hSerial);
        return 1;
    }
    csv << "Timestamp,Potentiometer,Button,Light\n";
    csv.flush();

    std::cout << "Port opened successfully.\n";
    std::cout << "Logging readings to " << csvFilename << "\n";
    std::cout << "Commands: R/G/B 1/0 (LEDs on/off), S000-S180 (servo angle), q (quit)\n";
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        redrawPromptLocked();
    }

    // Start the reader on its own thread so it can keep printing sensor
    // lines while main() polls for keyboard input below.
    std::thread reader(readerLoop, hSerial, std::ref(csv));

    // Main thread: poll for keystrokes non-blocking (via _kbhit/_getch)
    // instead of blocking on std::cin. This lets us own exactly when and
    // how typed characters hit the console, so the reader thread's prints
    // never land in the middle of a half-typed command.
    while (running) {
        if (_kbhit()) {
            char key = _getch();

            if (key == '\r') { // Enter key
                std::string submitted;
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    submitted = currentInput;
                    currentInput.clear();
                    std::cout << "\n";
                    redrawPromptLocked();
                }

                if (submitted == "q" || submitted == "Q") {
                    printLine("Quitting...\n");
                    running = false;
                    break;
                }

                if (!submitted.empty()) {
                    sendCommand(hSerial, submitted);
                }
            }
            else if (key == '\b') { // Backspace
                std::lock_guard<std::mutex> lock(consoleMutex);
                if (!currentInput.empty()) {
                    currentInput.pop_back();
                }
                // Erase the character visually, then redraw.
                std::cout << "\r> " << currentInput << " \b" << std::flush;
            }
            else if (isprint(static_cast<unsigned char>(key))) {
                std::lock_guard<std::mutex> lock(consoleMutex);
                currentInput += key;
                redrawPromptLocked();
            }
            // other control keys (arrows, etc.) are ignored for simplicity
        } else {
            // Avoid pegging a CPU core while idle.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Wait for the reader thread to notice `running == false` and exit
    // cleanly before we close the handle out from under it.
    reader.join();

    csv.close();
    CloseHandle(hSerial);

    return 0;
}