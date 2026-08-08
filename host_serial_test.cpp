#include <windows.h>  //windows functions/types include
#include <iostream>

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

    DCB dcbSerialParams = {0}; //devicecontrolblock
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = CBR_9600; // 9600bitsps
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);

    std::cout << "Port opened successfully.\n";

    //loop here calling ReadFile to pull in bytes

    CloseHandle(hSerial);
    return 0;
}