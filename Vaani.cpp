#include <windows.h>
#include <string>
#include <iostream>
#include <vector>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include "resource.h"

// --- Structure to hold device information ---
struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

HRESULT EnumerateAllEndpoints(std::vector<AudioDevice>& devices, bool isCapture);
std::string ExtractGuidFromEndpointId(const std::wstring& fullEndpointId);

constexpr const wchar_t* VAANIAPO_GUID = L"{B55E5A29-FE1E-4991-8079-4D0FE02015AF}";
constexpr const wchar_t* COMPOSITESFX_GUID = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
constexpr const wchar_t* COMPOSITEOSFX_GUID = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},19";
constexpr const wchar_t* BACKUP_REGPATH = L"SOFTWARE\\Vaani\\";
constexpr const char* DLL_NAME = "VaaniAPO.dll";
constexpr const wchar_t* disableAudioDgPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio";
constexpr const wchar_t* disableAudioDgKey = L"DisableProtectedAudioDG";

bool fxEnable = true;

std::wstring s2ws(const std::string& str) {
    std::wstring temp;
    int wcharsNum = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    temp.resize(wcharsNum);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &temp[0], wcharsNum);
    return temp;
}

std::string ws2s(const std::wstring& wStr) {
    std::string temp;
    int len = WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), wStr.size(), NULL, 0, 0, 0);
    temp.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), wStr.size(), &temp[0], len, 0, 0);
    return temp;
}

// Function to get the temporary file path (for extraction)
std::wstring GetDllPath(const std::wstring& dllName) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + dllName;
}

std::string GetLastErrorAsString() {
    // Get the error message ID, if any.
    DWORD errorMessageID = GetLastError();
    if (errorMessageID == 0) {
        return std::string(); // No error message has been recorded
    }

    LPSTR messageBuffer = nullptr;
    // Ask Windows to get the corresponding error string
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

    std::string message(messageBuffer, size);

    // Free the buffer allocated by FormatMessage
    LocalFree(messageBuffer);

    return message;
}

int ExtractDLL(const std::wstring& outputDll) {
    // 1. Find the embedded resource
    HRSRC hRes = FindResource(
        NULL,                            // Module handle for the current executable
        MAKEINTRESOURCE(IDR_RNNOISE_DLL),    // The resource ID
        L"BIN"                        // The resource type
    );

    if (!hRes) {
        std::wcerr << L"Error: Could not find resource." << std::endl;
        return -1;
    }

    // 2. Load the resource into memory
    HGLOBAL hGlobal = LoadResource(NULL, hRes);
    if (!hGlobal) {
        std::wcerr << L"Error: Could not load resource." << std::endl;
        return -1;
    }

    // 3. Get a pointer to the raw data and its size
    LPVOID pDllData = LockResource(hGlobal);
    DWORD dllSize = SizeofResource(NULL, hRes);

    // 4. Write the raw data to the specified file path
    std::filesystem::path outputDllPath = outputDll;
    std::filesystem::path outputDllPathParent = outputDllPath.parent_path();
    if (!std::filesystem::exists(outputDllPathParent)) {
        std::filesystem::create_directory(outputDllPathParent);
    }
    std::ofstream outfile(outputDll, std::ios::binary);
    if (outfile.is_open()) {
        outfile.write(static_cast<const char*>(pDllData), dllSize);
        outfile.close();
        std::wcout << L"DLL successfully extracted to: " << outputDll << std::endl;
    }
    else {
        std::wcerr << L"Error: Could not write DLL to disk." << std::endl;
        return -1;
    }

    return 0;
}

bool StopWinService(const std::wstring& serviceName) {
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    SERVICE_STATUS_PROCESS ssStatus;
    DWORD dwBytesNeeded;
    DWORD dwTimeout = 30000; // 30 seconds timeout
    DWORD dwStartTime = GetTickCount();

    std::wcout << L"Attempting to stop service: " << serviceName << std::endl;

    // Open a handle to the SCM database
    schSCManager = OpenSCManager(
        NULL,                 // Local computer
        SERVICES_ACTIVE_DATABASE, // Services active database
        SC_MANAGER_ALL_ACCESS // All access to SCM
    );

    if (NULL == schSCManager) {
        std::wcerr << L"OpenSCManager failed: " << GetLastErrorAsString().c_str() << std::endl;
        return false;
    }

    // Open a handle to the service
    schService = OpenService(
        schSCManager,           // SCM database
        serviceName.c_str(),    // Service name
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS
    );

    if (NULL == schService) {
        std::wcerr << L"OpenService failed (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Check if the service is already stopped
    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &dwBytesNeeded))
    {
        std::wcerr << L"QueryServiceStatusEx failed (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    if (ssStatus.dwCurrentState == SERVICE_STOPPED) {
        std::wcout << L"Service " << serviceName << L" is already stopped." << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return true;
    }

    // Send a stop code to the service
    if (!ControlService(
        schService,
        SERVICE_CONTROL_STOP,
        (LPSERVICE_STATUS)&ssStatus))
    {
        std::wcerr << L"ControlService (stop) failed (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Wait for the service to stop
    while (ssStatus.dwCurrentState != SERVICE_STOPPED) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ssStatus.dwWaitHint)); // Wait suggested by the service

        if (!QueryServiceStatusEx(
            schService,
            SC_STATUS_PROCESS_INFO,
            (LPBYTE)&ssStatus,
            sizeof(SERVICE_STATUS_PROCESS),
            &dwBytesNeeded))
        {
            std::wcerr << L"QueryServiceStatusEx failed during stop wait (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return false;
        }

        if (ssStatus.dwCurrentState == SERVICE_STOPPED) {
            std::wcout << L"Service " << serviceName << L" stopped successfully." << std::endl;
            break;
        }

        if (GetTickCount() - dwStartTime > dwTimeout) {
            std::wcerr << L"Service " << serviceName << L" stop timed out." << std::endl;
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return false;
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return true;
}

// Function to start a Windows service
bool StartWinService(const std::wstring& serviceName) {
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    SERVICE_STATUS_PROCESS ssStatus;
    DWORD dwBytesNeeded;
    DWORD dwTimeout = 30000; // 30 seconds timeout
    DWORD dwStartTime = GetTickCount();

    std::wcout << L"Attempting to start service: " << serviceName << std::endl;

    // Open a handle to the SCM database
    schSCManager = OpenSCManager(
        NULL,                 // Local computer
        SERVICES_ACTIVE_DATABASE, // Services active database
        SC_MANAGER_ALL_ACCESS // All access to SCM
    );

    if (NULL == schSCManager) {
        std::wcerr << L"OpenSCManager failed: " << GetLastErrorAsString().c_str() << std::endl;
        return false;
    }

    // Open a handle to the service
    schService = OpenService(
        schSCManager,           // SCM database
        serviceName.c_str(),    // Service name
        SERVICE_START | SERVICE_QUERY_STATUS
    );

    if (NULL == schService) {
        std::wcerr << L"OpenService failed (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Check if the service is already running
    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &dwBytesNeeded))
    {
        std::wcerr << L"QueryServiceStatusEx failed (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    if (ssStatus.dwCurrentState == SERVICE_RUNNING) {
        std::wcout << L"Service " << serviceName << L" is already running." << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return true;
    }

    // Start the service
    if (!StartService(
        schService,  // Handle to service
        0,           // No arguments
        NULL))       // No arguments
    {
        std::wcerr << L"StartService failed (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Wait for the service to start
    while (ssStatus.dwCurrentState != SERVICE_RUNNING) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ssStatus.dwWaitHint)); // Wait suggested by the service

        if (!QueryServiceStatusEx(
            schService,
            SC_STATUS_PROCESS_INFO,
            (LPBYTE)&ssStatus,
            sizeof(SERVICE_STATUS_PROCESS),
            &dwBytesNeeded))
        {
            std::wcerr << L"QueryServiceStatusEx failed during start wait (service: " << serviceName << L"): " << GetLastErrorAsString().c_str() << std::endl;
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return false;
        }

        if (ssStatus.dwCurrentState == SERVICE_RUNNING) {
            std::wcout << L"Service " << serviceName << L" started successfully." << std::endl;
            break;
        }

        if (GetTickCount() - dwStartTime > dwTimeout) {
            std::wcerr << L"Service " << serviceName << L" start timed out." << std::endl;
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return false;
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return true;
}

HRESULT EnumerateAllEndpoints(std::vector<AudioDevice>& devices, bool isCapture) {
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDeviceCollection* pCollection = NULL;
    IMMDevice* pDevice = NULL;
    IPropertyStore* pProps = NULL;
    LPWSTR wszId = NULL;
    PROPVARIANT varName;
    HRESULT hr;

    // Create the device enumerator
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator
    );

    if (FAILED(hr)) {
        std::cerr << "CoCreateInstance failed: " << hr << std::endl;
        return hr;
    }

    if (isCapture) {
        // Get the collection of audio capture endpoints
        hr = pEnumerator->EnumAudioEndpoints(
            eCapture,         // Enumerate capture devices
            DEVICE_STATE_ACTIVE, // Only active devices
            &pCollection
        );
    }
    else {
        // Get the collection of audio render endpoints
        hr = pEnumerator->EnumAudioEndpoints(
            eRender,         // Enumerate render devices
            DEVICE_STATE_ACTIVE, // Only active devices
            &pCollection
        );
    }

    if (FAILED(hr)) {
        std::cerr << "EnumAudioEndpoints failed: " << hr << std::endl;
        pEnumerator->Release();
        return hr;
    }

    UINT count;
    pCollection->GetCount(&count);

    if (count == 0) {
        std::cout << "(No active capture devices found)" << std::endl;
    }

    // Iterate through each device in the collection
    for (UINT i = 0; i < count; ++i) {
        // Get the i-th device
        hr = pCollection->Item(i, &pDevice);
        if (FAILED(hr)) { continue; }

        // Get the device ID string
        hr = pDevice->GetId(&wszId);
        if (FAILED(hr)) { pDevice->Release(); continue; }

        // Open the property store for the device
        hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
        if (FAILED(hr)) { CoTaskMemFree(wszId); pDevice->Release(); continue; }

        // Initialize PROPVARIANT for the device friendly name
        PropVariantInit(&varName);

        // Get the friendly name property
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        if (FAILED(hr)) {
            // If friendly name not available, use the ID as a fallback
            devices.push_back({ wszId, std::wstring(wszId) });
        }
        else {
            // Store the device ID and friendly name
            devices.push_back({ wszId, varName.pwszVal });
        }

        // Clean up COM objects for the current device
        PropVariantClear(&varName);
        if (pProps) pProps->Release();
        CoTaskMemFree(wszId);
        if (pDevice) pDevice->Release();
    }

    // Clean up COM objects for the collection and enumerator
    if (pCollection) pCollection->Release();
    if (pEnumerator) pEnumerator->Release();

    return S_OK;
}

std::string ExtractGuidFromEndpointId(const std::wstring& fullEndpointId) {
    std::string epIdStr = ws2s(fullEndpointId);

    size_t firstBrace1 = fullEndpointId.find('{');
    size_t lastBrace1 = fullEndpointId.find('}', firstBrace1);
    if (lastBrace1 == std::string::npos || lastBrace1 < firstBrace1) {
        return "";
    }

    size_t firstBrace2 = fullEndpointId.find('{', lastBrace1);
    size_t lastBrace2 = fullEndpointId.find('}', firstBrace2);

    // Extract the substring starting from the second opening brace
    // and with the calculated length of 38 characters.
    std::string extractedGuid = epIdStr.substr(firstBrace2, 38);

    return extractedGuid;
}

int Install() {
    HKEY hKeyFxProp = NULL;
    HKEY hKeyBackup = NULL;
    DWORD dwDisposition;
    DWORD dataSize = 0;
    LONG lResult;
    int disableAudioDGvalue = 1;
    std::wstring command, params;
    HINSTANCE hInst;

    HRESULT ret = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(ret)) {
        // Enumerate all capture endpoints
        std::vector<AudioDevice> devices;
        std::cout << "\nAvailable Capture Endpoints:" << std::endl;
        auto res = EnumerateAllEndpoints(devices, true); // Capture endpoints only
        if (FAILED(res)) {
            std::cerr << "Error enumerating capture endpoints: " << res << std::endl;
            CoUninitialize();
            return 1;
        }

        // List all and get user input of their desired endpoint
        for (size_t i = 0; i < devices.size(); ++i) {
            wprintf(L"%zu: %ls\n", i + 1, devices[i].name.c_str());
        }

        int micChoice = -1;
        std::cout << "Select the microphone to install on: ";
        std::cin >> micChoice;

        if (std::cin.fail() || micChoice < 1 || micChoice > static_cast<int>(devices.size())) {
            std::cerr << "Invalid selection." << std::endl;
            CoUninitialize();
            return 1;
        }

        if (devices.empty()) {
            std::cout << "No microphones found." << std::endl;
            CoUninitialize();
            return 1;
        }

        AudioDevice selectedDevice = devices[micChoice - 1];
        std::string audioEndpointGuidStr = ExtractGuidFromEndpointId(selectedDevice.id);
        std::cout << "Endpoint selected: " << ws2s(selectedDevice.name) << std::endl;
        std::cout << "Device ID: " << audioEndpointGuidStr << std::endl << std::endl;

        // Get user input for the APO chain they want to install in
        std::wstring targetApoGuid, targetApoGuidOffload;
        std::string targetApo;
        targetApo = "SFX";
        targetApoGuid = COMPOSITESFX_GUID;
        targetApoGuidOffload = COMPOSITEOSFX_GUID;

        // Construct the full Capture registry path
        std::string subKeyPath = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
        subKeyPath += audioEndpointGuidStr; // Append the user-provided audio endpoint GUID
        subKeyPath += "\\FxProperties";
        std::wstring wSubKeyPath = s2ws(subKeyPath);
        std::wcout << L"Selected Endpoint Registry: " << wSubKeyPath << std::endl;

        // Construct the full backup registry path
        std::wstring backupRegPath = BACKUP_REGPATH + s2ws(audioEndpointGuidStr);
        DWORD originalDataType = 0;
        std::vector<BYTE> originalData;

        /** Prep: Done doing housekeeping, now copy all deps to ProgramData for installation **/
        std::wstring destinationDir = L"C:\\ProgramData\\Vaani\\";
        int ret = ExtractDLL(destinationDir + s2ws(DLL_NAME));
        if (ret == -1) {
            return 1;
        }

        // Register the Vaani APO DLL
        command = L"regsvr32.exe";
        params = L"/s \"" + s2ws((ws2s(destinationDir) + DLL_NAME)) + L"\"";
        hInst = ShellExecuteW(
            NULL,
            NULL,
            command.c_str(),
            params.c_str(),
            NULL,
            SW_HIDE
        );

        /** 1. Create/Open the new backup registry key in **/
        lResult = RegCreateKeyEx(
            HKEY_CURRENT_USER,
            backupRegPath.c_str(),
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_READ,
            NULL,
            &hKeyBackup,
            &dwDisposition
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyBackup);
            CoUninitialize();
            goto exit;
        }

        /** 2. Open the FxProperties of the chosen endpoint **/
        lResult = RegOpenKeyEx(
            HKEY_LOCAL_MACHINE,
            wSubKeyPath.c_str(),
            0,
            KEY_READ,
            &hKeyFxProp
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp);
            CoUninitialize();
            goto exit;
        }

        /** 3. Backup existing GUID keys from the the selected APO of the selected endpoint GUID **/
        std::cout << "Attempting to backup existing registry value..." << std::endl;
        lResult = RegQueryValueEx(
            hKeyFxProp,
            targetApoGuid.c_str(),
            NULL,
            &originalDataType,
            NULL,
            &dataSize
        );
        if (lResult == ERROR_SUCCESS) {
            // Allocate buffer and get the data
            originalData.resize(dataSize);
            lResult = RegQueryValueEx(
                hKeyFxProp,
                targetApoGuid.c_str(),
                NULL,
                &originalDataType,
                originalData.data(),
                &dataSize
            );
            if (lResult != ERROR_SUCCESS) {
                RegCloseKey(hKeyFxProp);
                CoUninitialize();
                goto exit;
            }

            // Set the data to the backup registry path **/
            lResult = RegSetValueEx(
                hKeyBackup,
                targetApoGuid.c_str(),
                0,
                originalDataType,
                originalData.data(),
                originalData.size()
            );
            if (lResult != ERROR_SUCCESS) {
                RegCloseKey(hKeyBackup); // Close the registry key before exiting
                CoUninitialize();
                goto exit;
            }

            if (!targetApoGuidOffload.empty()) {
                // Backup the offload registries as well if not empty
                originalData.clear();
                lResult = RegQueryValueEx(
                    hKeyFxProp,
                    targetApoGuidOffload.c_str(),
                    NULL,
                    &originalDataType,
                    NULL,
                    &dataSize
                );
                if (lResult == ERROR_SUCCESS) {
                    // Allocate buffer and get the data
                    originalData.resize(dataSize);
                    lResult = RegQueryValueEx(
                        hKeyFxProp,
                        targetApoGuidOffload.c_str(),
                        NULL,
                        &originalDataType,
                        originalData.data(),
                        &dataSize
                    );
                    if (lResult != ERROR_SUCCESS) {
                        RegCloseKey(hKeyFxProp);
                        CoUninitialize();
                        goto exit;
                    }

                    // Set the data to the backup registry path **/
                    lResult = RegSetValueEx(
                        hKeyBackup,
                        targetApoGuidOffload.c_str(),
                        0,
                        originalDataType,
                        originalData.data(),
                        originalData.size()
                    );
                    if (lResult != ERROR_SUCCESS) {
                        RegCloseKey(hKeyBackup); // Close the registry key before exiting
                        CoUninitialize();
                        goto exit;
                    }
                }
            }

            std::cout << std::endl << "Backed up original APO keys to \"HKEY_CURRENT_USER\\" << ws2s(BACKUP_REGPATH) << "\" successfully." << std::endl;
        }
        else {
            std::cout << std::endl << "No previous " << targetApo << " value found. Directly setting the registry value." << std::endl;
        }

        /** Write the registry key with our GUID value **/
        std::vector<std::wstring> multiStrings;
        multiStrings.push_back(VAANIAPO_GUID); // Future-proof for when we want to append the GUIDs

        size_t totalBufferSize = 0;
        for (const auto& s : multiStrings) {
            totalBufferSize += (s.size() + 1); // +1 for the null terminator of each string
        }
        totalBufferSize += 1; // +1 for the final double-null terminator

        // Buffer to hold the combined multi-string data
        std::vector<WCHAR> multiSzBuffer(totalBufferSize);
        WCHAR* currentPtr = multiSzBuffer.data();

        for (const auto& s : multiStrings) {
            wcscpy_s(currentPtr, multiSzBuffer.size() - (currentPtr - multiSzBuffer.data()), s.c_str());
            currentPtr += (s.size() + 1);
        }
        *currentPtr = L'\0';

        std::cout << std::endl << "Installing Vaani..." << std::endl;
        lResult = RegCreateKeyEx(
            HKEY_LOCAL_MACHINE,
            wSubKeyPath.c_str(),
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            NULL,
            &hKeyFxProp,
            &dwDisposition
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyBackup);
            CoUninitialize();
            goto exit;
        }

        lResult = RegSetValueEx(
            hKeyFxProp,
            targetApoGuid.c_str(),
            0,
            REG_MULTI_SZ,
            (const BYTE*)multiSzBuffer.data(),
            multiSzBuffer.size() * sizeof(WCHAR)
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp); // Close the registry key before exiting
            CoUninitialize();
            goto exit;
        }

        // Set our GUID to its offload pin as well
        lResult = RegSetValueEx(
            hKeyFxProp,
            targetApoGuidOffload.c_str(),
            0,
            REG_MULTI_SZ,
            (const BYTE*)multiSzBuffer.data(),
            multiSzBuffer.size() * sizeof(WCHAR)
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp); // Close the registry key before exiting
            CoUninitialize();
            goto exit;
        }
        std::cout << std::endl << "Successfully installed Vaani." << std::endl;

        // Disable the protected key to run unsigned APOs
        lResult = RegCreateKeyEx(
            HKEY_LOCAL_MACHINE,
            disableAudioDgPath,
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            NULL,
            &hKeyFxProp,
            NULL
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp);
            CoUninitialize();
            goto exit;
        }

        lResult = RegSetValueEx(
            hKeyFxProp,
            disableAudioDgKey,
            0,
            REG_DWORD,
            (const BYTE*)&disableAudioDGvalue,
            sizeof(disableAudioDGvalue)
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp);
            CoUninitialize();
            goto exit;
        }

        RegCloseKey(hKeyFxProp);
        RegCloseKey(hKeyBackup);
        CoUninitialize();
    }

    // Reconfigure the audio service to allow starting it in a new thread after reboot
    command = L"sc.exe ";
    params = L"config audiosrv type= own";
    hInst = ShellExecuteW(
        NULL,
        NULL,
        command.c_str(),
        params.c_str(),
        NULL,
        SW_HIDE
    );

    std::cout << std::endl << "It is recommended to reboot your system for all changes to take full effect." << std::endl;
exit:
    system("pause");
    return 0;
}

int Uninstall() {
    int disableAudioDGvalue = 0;
    HRESULT ret = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (SUCCEEDED(ret)) {
        std::string audioEndpointGuidStr;
        std::vector<std::wstring> endpointsList;

        HKEY hKeyFxProp = NULL;
        HKEY hKeyBackup = NULL;
        LONG lResult;
        bool isCapture = true; // Choose either render or capture

        // Open backup registry path
        lResult = RegOpenKeyEx(
            HKEY_CURRENT_USER,
            BACKUP_REGPATH,
            0,
            KEY_READ,
            &hKeyBackup
        );
        if (lResult == ERROR_SUCCESS) {
            std::cout << "Backup found. Restoring values from backup...\n";
            TCHAR subkeyName[256];
            DWORD subkeyNameSize = sizeof(subkeyName) / sizeof(subkeyName[0]);
            DWORD index = 0;

            // Enumerate subkeys
            std::cout << "Endpoints Vaani is applied to: " << std::endl;
            while (RegEnumKeyEx(hKeyBackup, index, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                std::wcout << subkeyName << std::endl;
                index++;
                subkeyNameSize = sizeof(subkeyName) / sizeof(subkeyName[0]); // Reset the size for next call
                endpointsList.push_back(subkeyName);
            }
        }
        else {
            // How are we here?
            RegCloseKey(hKeyBackup);
            CoUninitialize();
            std::cout << "Vaani is not installed. Exiting uninstaller." << std::endl;
            system("pause");
            return 1;
        }

        for (auto& endpoint : endpointsList) {
            // Build the target subkey path in FxProperties
            std::wstring wSubKeyPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
            wSubKeyPath += endpoint + L"\\FxProperties";
            std::wstring wBackupSubKeyPath = BACKUP_REGPATH + endpoint;

            // Open backup key path for reading
            lResult = RegOpenKeyEx(
                HKEY_CURRENT_USER,
                wBackupSubKeyPath.c_str(),
                0,
                KEY_READ,
                &hKeyBackup
            );
            if (lResult != ERROR_SUCCESS) {
                RegCloseKey(hKeyBackup);
                CoUninitialize();
                system("pause");
                return 1;
            }

            // Open FxProperty key for writing (to restore values)
            lResult = RegCreateKeyEx(
                HKEY_LOCAL_MACHINE,
                wSubKeyPath.c_str(),
                0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                &hKeyFxProp, NULL
            );
            if (lResult != ERROR_SUCCESS) {
                RegCloseKey(hKeyFxProp);
                RegCloseKey(hKeyBackup);
                CoUninitialize();
                system("pause");
                return 1;
            }

            DWORD dataSize = 0;
            DWORD originalDataType = 0;
            std::vector<BYTE> originalData;
            std::wstring wTargetApoChain, wTargetApoOffloadChain;
            wTargetApoChain = COMPOSITESFX_GUID;
            wTargetApoOffloadChain = COMPOSITEOSFX_GUID;

            // Get the target APO GUID data and set to the same FxProperties registry path **/
            lResult = RegQueryValueEx(
                hKeyBackup,
                wTargetApoChain.c_str(),
                NULL,
                &originalDataType,
                NULL,
                &dataSize
            );
            if (lResult == ERROR_SUCCESS) {
                originalData.clear();
                originalData.resize(dataSize);
                lResult = RegQueryValueEx(
                    hKeyBackup,
                    wTargetApoChain.c_str(),
                    NULL,
                    &originalDataType,
                    originalData.data(),
                    &dataSize
                );
                if (lResult != ERROR_SUCCESS) {
                    RegCloseKey(hKeyFxProp);
                    RegCloseKey(hKeyBackup);
                    CoUninitialize();
                    system("pause");
                    return 1;
                }

                lResult = RegSetValueEx(
                    hKeyFxProp,
                    wTargetApoChain.c_str(),
                    0,
                    originalDataType,
                    originalData.data(),
                    originalData.size()
                );
                if (lResult != ERROR_SUCCESS) {
                    RegCloseKey(hKeyFxProp);
                    RegCloseKey(hKeyBackup);
                    CoUninitialize();
                    system("pause");
                    return 1;
                }

                // Restore the offload path chain information as well if available
                if (!wTargetApoOffloadChain.empty()) {
                    lResult = RegQueryValueEx(
                        hKeyBackup,
                        wTargetApoOffloadChain.c_str(),
                        NULL,
                        &originalDataType,
                        NULL,
                        &dataSize
                    );
                    if (lResult == ERROR_SUCCESS) {
                        originalData.clear();
                        originalData.resize(dataSize);
                        lResult = RegQueryValueEx(
                            hKeyBackup,
                            wTargetApoOffloadChain.c_str(),
                            NULL,
                            &originalDataType,
                            originalData.data(),
                            &dataSize
                        );
                        if (lResult != ERROR_SUCCESS) {
                            RegCloseKey(hKeyFxProp);
                            RegCloseKey(hKeyBackup);
                            CoUninitialize();
                            system("pause");
                            return 1;
                        }

                        lResult = RegSetValueEx(
                            hKeyFxProp,
                            wTargetApoOffloadChain.c_str(),
                            0,
                            originalDataType,
                            originalData.data(),
                            originalData.size()
                        );
                        if (lResult != ERROR_SUCCESS) {
                            RegCloseKey(hKeyFxProp);
                            RegCloseKey(hKeyBackup);
                            CoUninitialize();
                            system("pause");
                            return 1;
                        }
                    }
                }
            }
            else {
                lResult = RegDeleteValue(
                    hKeyFxProp,
                    wTargetApoChain.c_str()
                );
                if (lResult != ERROR_SUCCESS) {
                    RegCloseKey(hKeyFxProp);
                    RegCloseKey(hKeyBackup);
                    CoUninitialize();
                    system("pause");
                    return 1;
                }

                if (!wTargetApoOffloadChain.empty()) {
                    lResult = RegDeleteValue(
                        hKeyFxProp,
                        wTargetApoOffloadChain.c_str()
                    );
                    if (lResult != ERROR_SUCCESS) {
                        RegCloseKey(hKeyFxProp);
                        RegCloseKey(hKeyBackup);
                        CoUninitialize();
                        system("pause");
                        return 1;
                    }
                }
            }
        }

        // Re-enable the protected APO registry key
        lResult = RegCreateKeyEx(
            HKEY_LOCAL_MACHINE,
            disableAudioDgPath,
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            NULL,
            &hKeyFxProp,
            NULL
        );
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp);
            CoUninitialize();
            return 1;
        }

        lResult = RegDeleteValueW(hKeyFxProp, disableAudioDgKey);
        if (lResult != ERROR_SUCCESS) {
            RegCloseKey(hKeyFxProp);
            CoUninitialize();
            return 1;
        }

        // Close handles
        if (hKeyFxProp != NULL) RegCloseKey(hKeyFxProp);
        RegCloseKey(hKeyBackup);

        // Delete the backup key after successful restoration
        std::cout << std::endl << "Deleting backups..." << std::endl;
        lResult = RegDeleteTreeW(HKEY_CURRENT_USER, BACKUP_REGPATH);
        if (lResult == ERROR_SUCCESS) {
            std::cout << "Backups deleted successfully.\n";
        }
        else {
            std::cout << "Error deleting backups. You can remove the entries manually.\n";
        }

        // Unregister RNNoiseAPO.dll
        std::wstring destinationDir = L"C:\\ProgramData\\Vaani\\";
        std::wstring command = L"regsvr32.exe";
        std::wstring params = L"/s /u \"" + (destinationDir + s2ws(DLL_NAME)) + L"\"";
        HINSTANCE hInst = ShellExecuteW(
            NULL,
            NULL,
            command.c_str(),
            params.c_str(),
            NULL,
            SW_HIDE
        );

        CoUninitialize();

        // Stop the Windows Audio Service
        const std::wstring serviceName = L"AudioSrv";
        if (!StopWinService(serviceName)) {
            std::wcerr << L"\nFailed to stop Windows Audio service." << std::endl;
        }

        // Delete all installed remnants from ProgramData
        try {
            if (!std::filesystem::exists(destinationDir)) {
                return 1;
            }
            std::filesystem::remove_all(destinationDir);
        }
        catch (const std::filesystem::filesystem_error& ex) {
            return 1;
        }

        // Restart Windows Audio Service
        if (!StartWinService(serviceName)) {
            std::wcerr << L"\nFailed to restart Windows Audio service." << std::endl;
        }
    }

    std::cout << "Vaani uninstalled successfully." << std::endl;
    system("pause");
    return 0;
}


bool SetApoProcessingState(bool bEnable) {
    HKEY hKey;

    // Open key with write permissions
    LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, BACKUP_REGPATH, 0, KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS) {
        std::wcout << "Vaani not installed. Please install Vaani before running this command." << std::endl;
        return false;
    }

    DWORD dwValue = bEnable ? 1 : 0;
    result = RegSetValueEx(hKey, L"FxEnable", 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));

    RegCloseKey(hKey);
    return (result == ERROR_SUCCESS);
}

// Application Entry //
int main() {
    int choice;
    std::cout << "\nVaani: RNNoise based Voice Clarity \n1) Install\t2) Uninstall\n3) Enable all effects\t4) Disable all effects" << std::endl;
    std::cin >> choice;

    switch (choice) {
    case 1:
        Install();
        break;
    case 2:
        Uninstall();
        break;
    case 3:
        SetApoProcessingState(true);
        break;
    case 4:
        SetApoProcessingState(false);
        break;
    default:
        CoUninitialize();
        return 1;
    }
}