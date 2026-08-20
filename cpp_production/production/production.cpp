// production.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
#include <winhttp.h>
#include <setupapi.h>
#include <devguid.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "setupapi.lib")

// Accepts "5" or "COM5" (case-insensitive); returns true and sets outNumber (1-99) on success.
static bool TryParseComPortNumber(const std::string& text, int& outNumber)
{
    std::string digits = text;
    if (digits.size() > 3)
    {
        std::string prefix = digits.substr(0, 3);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::toupper);
        if (prefix == "COM")
        {
            digits = digits.substr(3);
        }
    }

    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
    {
        return false;
    }

    int value = std::stoi(digits);
    if (value < 1 || value > 99)
    {
        return false;
    }

    outNumber = value;
    return true;
}

// QueryDosDevice succeeds for COMx only if that device actually exists on the system.
static std::vector<int> EnumerateAvailableComPorts()
{
    std::vector<int> ports;
    char target[256];

    for (int i = 1; i <= 99; ++i)
    {
        std::string deviceName = "COM" + std::to_string(i);
        if (QueryDosDeviceA(deviceName.c_str(), target, sizeof(target)) != 0)
        {
            ports.push_back(i);
        }
    }

    return ports;
}

static bool FileExists(const std::string& path)
{
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Proper wide->narrow conversion (avoids silently truncating each wchar_t to a char).
static std::string NarrowFromWide(const std::wstring& wide)
{
    if (wide.empty())
    {
        return std::string();
    }

    int sizeNeeded = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), NULL, 0, NULL, NULL);
    std::string narrow(sizeNeeded, '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), &narrow[0], sizeNeeded, NULL, NULL);
    return narrow;
}

// Maps COM port number -> friendly device name (e.g. "STMicroelectronics STLink Virtual COM Port"),
// read from the "Ports (COM & LPT)" device class. Ports with no friendly name (or not in that class)
// are simply absent from the returned map.
static std::map<int, std::string> QueryComPortFriendlyNames()
{
    std::map<int, std::string> names;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        return names;
    }

    SP_DEVINFO_DATA devInfoData = { 0 };
    devInfoData.cbSize = sizeof(devInfoData);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); ++i)
    {
        wchar_t friendlyNameW[512] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_FRIENDLYNAME, NULL,
            reinterpret_cast<PBYTE>(friendlyNameW), sizeof(friendlyNameW) - sizeof(wchar_t), NULL))
        {
            continue;
        }

        // Friendly names look like "STMicroelectronics STLink Virtual COM Port (COM3)" -
        // pull the port number out of the trailing "(COMx)" and use the rest as the display name.
        std::wstring friendlyName(friendlyNameW);

        size_t openParen = friendlyName.rfind(L"(COM");
        if (openParen == std::wstring::npos)
        {
            continue;
        }
        size_t closeParen = friendlyName.find(L')', openParen);
        if (closeParen == std::wstring::npos)
        {
            continue;
        }

        std::wstring portDigits = friendlyName.substr(openParen + 4, closeParen - (openParen + 4));
        if (portDigits.empty() || !std::all_of(portDigits.begin(), portDigits.end(), ::iswdigit))
        {
            continue;
        }

        std::wstring nameOnly = friendlyName.substr(0, openParen);
        while (!nameOnly.empty() && nameOnly.back() == L' ')
        {
            nameOnly.pop_back();
        }

        names[std::stoi(portDigits)] = NarrowFromWide(nameOnly);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return names;
}

// Flashes complete_fw.bin via esptool.exe. Both are expected in the current working
// directory - this tool is always launched from its own folder, so no path resolution
// is needed. Chip type is left for esptool to auto-detect rather than hardcoded, so
// this doesn't need updating if a different ESP32 variant is used later. esptool's own
// console output streams straight through (child inherits this process's console, same
// as running it by hand) rather than being captured, for the same "show everything"
// visibility as HttpsGetClaimResult's request/response dump.
static bool RunEsptoolWriteFlash(int comPort)
{
    if (!FileExists("esptool.exe"))
    {
        std::cerr << "esptool.exe not found in the current directory\n";
        return false;
    }
    if (!FileExists("complete_fw.bin"))
    {
        std::cerr << "complete_fw.bin not found in the current directory\n";
        return false;
    }

    std::string cmdLine = "esptool.exe --port COM" + std::to_string(comPort) +
        " --baud 921600 write_flash 0x0 complete_fw.bin";

    // CreateProcessA may write into this buffer, so a std::string's own storage
    // (or a string literal) isn't safe to pass directly as lpCommandLine.
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    std::cout << "--- Flashing firmware ---\n\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n!!!!!!!!MAKE SUERE THAT D0 is connected to GND!!!!!!!!\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n" << cmdLine << "\n";

    STARTUPINFOA startupInfo = { sizeof(startupInfo) };
    PROCESS_INFORMATION processInfo = { 0 };

    BOOL created = CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL,
        &startupInfo, &processInfo);
    if (!created)
    {
        std::cerr << "Failed to launch esptool.exe (error " << GetLastError() << ")\n";
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    std::cout << "--- esptool exited with code " << exitCode << " ---\n";

    return exitCode == 0;
}

static HANDLE OpenComPort(int portNumber)
{
    // "\\.\COMx" works for both single- and double-digit port numbers.
    std::string path = "\\\\.\\COM" + std::to_string(portNumber);

    HANDLE hSerial = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSerial == INVALID_HANDLE_VALUE)
    {
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hSerial, &dcb))
    {
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(hSerial, &dcb))
    {
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    // All-zero COMMTIMEOUTS makes ReadFile block until the requested bytes arrive.
    COMMTIMEOUTS timeouts = { 0 };
    SetCommTimeouts(hSerial, &timeouts);

    return hSerial;
}

// Retries OpenComPort() for up to totalTimeoutMs. Needed right after
// RunEsptoolWriteFlash(): the XIAO ESP32-S3's native USB means the post-flash reset can
// make the port briefly disappear and re-enumerate rather than just staying put, so the
// very next open attempt can spuriously fail if it lands in that gap.
static HANDLE OpenComPortWithRetry(int portNumber, DWORD totalTimeoutMs)
{
    const DWORD retryIntervalMs = 500;
    DWORD elapsedMs = 0;

    for (;;)
    {
        HANDLE hSerial = OpenComPort(portNumber);
        if (hSerial != INVALID_HANDLE_VALUE)
        {
            return hSerial;
        }

        if (elapsedMs >= totalTimeoutMs)
        {
            return INVALID_HANDLE_VALUE;
        }

        Sleep(retryIntervalMs);
        elapsedMs += retryIntervalMs;
    }
}

// Blocks until a full line (terminated by '\n') arrives; strips a trailing '\r'.
static bool ReadLineFromSerial(HANDLE hSerial, std::string& outLine)
{
    std::string line;
    char ch = 0;
    DWORD bytesRead = 0;

    while (ReadFile(hSerial, &ch, 1, &bytesRead, NULL) && bytesRead == 1)
    {
        if (ch == '\n')
        {
            outLine = line;
            return true;
        }
        if (ch != '\r')
        {
            line += ch;
        }
    }

    return false;
}

// Validates "0x" + the 5-byte sync marker (C0BEC0B0B0) + an 18-hex-char device id
// (see g_ui8_usb_id_magic / print_device_id_over_usb() in wifi_bt.ino). Returns the
// full trimmed line - claim_device_alx.php expects the marker included in "id".
static bool TryParseDeviceId(const std::string& rawLine, std::string& outId)
{
    std::string trimmed = rawLine;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
    {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
    {
        trimmed.pop_back();
    }

    static const std::string kPrefix = "0XC0BEC0B0B0";
    static const size_t kIdHexLength = 18;

    if (trimmed.size() != kPrefix.size() + kIdHexLength)
    {
        return false;
    }

    std::string upper = trimmed;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    if (upper.compare(0, kPrefix.size(), kPrefix) != 0)
    {
        return false;
    }

    std::string idHex = upper.substr(kPrefix.size());
    if (!std::all_of(idHex.begin(), idHex.end(), [](unsigned char c) { return std::isxdigit(c); }))
    {
        return false;
    }

    outId = trimmed;
    return true;
}

// Returns the raw response headers (status line + headers, CRLF-separated) as a narrow string.
static std::string QueryRawResponseHeaders(HINTERNET hRequest)
{
    DWORD headersSize = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER, &headersSize, WINHTTP_NO_HEADER_INDEX);
    if (headersSize == 0)
    {
        return std::string();
    }

    std::wstring rawHeaders(headersSize / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        &rawHeaders[0], &headersSize, WINHTTP_NO_HEADER_INDEX))
    {
        return std::string();
    }

    // Headers are effectively ASCII here, so a plain widen/narrow round trip is fine.
    return NarrowFromWide(rawHeaders);
}

// HTTPS GET to claim_device_alx.php?id=<deviceId>; outResponse gets the plain-text body ("OK" or "ERROR: ...").
// Prints the full request/response (URL, status, headers, body) to stdout for visibility.
static bool HttpsGetClaimResult(const std::string& deviceId, std::string& outResponse)
{
    std::string urlForLog = "https://alxlabs.ca/books/env_sensor/web/claim_device_alx.php?id=" + deviceId;
    std::wstring path = L"/books/env_sensor/web/claim_device_alx.php?id=" + std::wstring(deviceId.begin(), deviceId.end());

    std::cout << "--- HTTPS request ---\n";
    std::cout << "GET " << urlForLog << " HTTP/1.1\n";
    std::cout << "Host: alxlabs.ca\n";

    HINTERNET hSession = WinHttpOpen(L"production/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        std::cerr << "WinHttpOpen failed (error " << GetLastError() << ")\n";
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, L"alxlabs.ca", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect)
    {
        std::cerr << "WinHttpConnect failed (error " << GetLastError() << ")\n";
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        std::cerr << "WinHttpOpenRequest failed (error " << GetLastError() << ")\n";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
    if (!ok)
    {
        std::cerr << "WinHttpSendRequest failed (error " << GetLastError() << ")\n";
    }

    ok = ok && WinHttpReceiveResponse(hRequest, NULL) != FALSE;
    if (!ok)
    {
        std::cerr << "WinHttpReceiveResponse failed (error " << GetLastError() << ")\n";
    }

    if (ok)
    {
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

        std::cout << "--- HTTPS response ---\n";
        std::cout << "HTTP status: " << statusCode << "\n";
        std::cout << QueryRawResponseHeaders(hRequest);

        std::string body;
        DWORD available = 0;
        do
        {
            available = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0)
            {
                break;
            }

            std::vector<char> buffer(available);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, buffer.data(), available, &bytesRead))
            {
                break;
            }
            body.append(buffer.data(), bytesRead);
        } while (available > 0);

        std::cout << "--- HTTPS body ---\n" << body << "\n----------------------\n";

        outResponse = body;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return ok;
}

int main(int argc, char* argv[])
{
    int comPort = 0;

    if (argc > 1)
    {
        if (!TryParseComPortNumber(argv[1], comPort))
        {
            std::cerr << "Invalid COM port parameter: " << argv[1] << " (expected COM1-COM99)\n";
            return 1;
        }
    }
    else
    {
        system("cls");

        std::vector<int> availablePorts = EnumerateAvailableComPorts();
        std::map<int, std::string> portNames = QueryComPortFriendlyNames();

        std::cout << "Available COM ports:\n";
        if (availablePorts.empty())
        {
            std::cout << "  (none found)\n";
        }
        else
        {
            for (int port : availablePorts)
            {
                std::cout << "  COM" << port;

                auto nameIt = portNames.find(port);
                if (nameIt != portNames.end())
                {
                    std::cout << " - " << nameIt->second;
                }

                std::cout << "\n";
            }
        }

        std::cout << "Select COM port: ";
        std::string input;
        std::getline(std::cin, input);

        if (!TryParseComPortNumber(input, comPort))
        {
            std::cerr << "Invalid COM port: " << input << "\n";
            return 1;
        }
    }

    if (!RunEsptoolWriteFlash(comPort))
    {
        std::cerr << "Firmware flash failed - aborting\n";
        return 1;
    }

    std::cout << "Reopening COM" << comPort << " after flash (device may take a moment to re-enumerate)...\n";
    HANDLE hSerial = OpenComPortWithRetry(comPort, 10000);
    if (hSerial == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Failed to open COM" << comPort << "\n";
        return 1;
    }

    std::cout << "Waiting for device id on COM" << comPort << "...\n";

    std::string deviceId;
    for (;;)
    {
        std::string line;
        if (!ReadLineFromSerial(hSerial, line))
        {
            CloseHandle(hSerial);
            std::cerr << "Failed to read from COM" << comPort << "\n";
            return 1;
        }

        if (TryParseDeviceId(line, deviceId))
        {
            break;
        }

        std::cout << line << "\n";
    }

    CloseHandle(hSerial);

    std::string response;
    if (!HttpsGetClaimResult(deviceId, response))
    {
        std::cerr << "Failed to reach claim server\n";
        return 1;
    }

    while (!response.empty() && (response.back() == '\r' || response.back() == '\n'))
    {
        response.pop_back();
    }

    std::cout << response << "\n";

    return 0;
}
