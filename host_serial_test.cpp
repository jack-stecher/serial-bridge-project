#include <windows.h>  //windows functions/types include
#include <iostream>
#include <string>
#include <optional>
#include <conio.h>    // _kbhit() / _getch(), used for the exit key


struct SensorReading {
    int potentiometer;
    int buttonPressed;
    int lightLevel;
};

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

int main() {
    HANDLE hSerial = CreateFileA (      // handle to connect to COM3
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

    std::cout << "Port opened successfully.\n";
    std::cout << "Press 'q' at any time to quit.\n";

    // read sensor data

    std::string buffer;

    char byte;
    DWORD bytesRead;

    bool running = true;

    while (running) {

        // Check for the quit key without blocking. This works because
        // ReadFile below has timeouts configured, so it returns roughly
        // every 50ms even when no data has arrived -- the loop keeps
        // cycling back here to check for a keypress instead of stalling.
        if (_kbhit()) {
            char key = _getch();
            if (key == 'q' || key == 'Q') {
                std::cout << "\nQuitting...\n";
                running = false;
                break;
            }
        }

        // Read one byte from Arduino
        if (ReadFile(
            hSerial,
            &byte,
            1,
            &bytesRead,
            nullptr
        )) {

            if (bytesRead > 0) {

                // Add byte to our line buffer
                buffer += byte;

                // Arduino sent a complete line
                if (byte == '\n') {

                    // Remove the newline
                    buffer.pop_back();

                    // Remove \r if Arduino sends \r\n
                    if (!buffer.empty() && buffer.back() == '\r') {
                        buffer.pop_back();
                    }

                    // Parse the completed line
                    std::optional<SensorReading> reading = parseLine(buffer);

                    if (reading.has_value()) {
                        // Print the sensor values
                        std::cout
                            << "Potentiometer: " << reading->potentiometer
                            << " | Button: " << reading->buttonPressed
                            << " | Light: " << reading->lightLevel
                            << std::endl;
                    } else {
                        // Malformed line -- skip it and keep going rather
                        // than crashing or printing garbage.
                        std::cout << "Skipped malformed line.\n";
                    }

                    // Clear buffer for next line
                    buffer.clear();
                }
            }
        } else {
            // ReadFile itself failed (e.g. Arduino unplugged mid-run).
            std::cout << "Read error -- check the connection.\n";
            running = false;
        }
    }

//    Arduino
//    |
//    |  "512,1,734\n"
//    v
//     COM3
//    |
//    v
//    C++ program
//    |
//    +--> ReadFile()
//    |
//    +--> buffer
//    |
//    +--> parseLine()
//    |
//    +--> SensorReading struct
//    |
//    v
//    Potentiometer: 512 | Button: 1 | Light: 734

    CloseHandle(hSerial);

    return 0;
}