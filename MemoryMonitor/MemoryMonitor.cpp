#include <iostream>
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// 前向声明
class HealthMonitorController;

// DLL导出宏
#ifdef _WIN32
#ifdef MEMORYMONITOR_EXPORTS
#define MEMORYMONITOR_API __declspec(dllexport)
#else
#define MEMORYMONITOR_API __declspec(dllimport)
#endif
#else
#define MEMORYMONITOR_API
#endif

// 【新增】定义一个结构体，用于封装1P/2P的两种位置信息
struct PlayerPositions
{
    int netPosition;
    int localPosition;
};

// 定义回调类型，C# 将实现这个回调
typedef void(__stdcall *HealthChangedCallback)(int playerId, int newHealth, int oldHealth);

// 全局监控器实例和回调函数指针
static std::unique_ptr<HealthMonitorController> g_monitorController;
static HealthChangedCallback g_healthCallback = nullptr;

class MemoryReaderException : public std::runtime_error
{
public:
    MemoryReaderException(const std::string &message, DWORD errorCode = 0)
        : std::runtime_error(message + " (Error: " + std::to_string(errorCode) + ")"),
          errorCode(errorCode) {}
    DWORD getErrorCode() const { return errorCode; }

private:
    DWORD errorCode;
};

class ProcessHandle
{
public:
    ProcessHandle(DWORD pid, DWORD desireAccess) : handle(OpenProcess(desireAccess, FALSE, pid))
    {
        if (!handle)
        {
            throw MemoryReaderException("Failed to open process", GetLastError());
        }
    }
    ~ProcessHandle()
    {
        if (handle)
        {
            CloseHandle(handle);
        }
    }
    HANDLE get() const
    {
        return handle;
    }
    template <typename T>
    T readMemory(uintptr_t address) const
    {
        T value;
        if (!ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), nullptr))
        {
            throw MemoryReaderException("Faild to read memory", GetLastError());
        }
        return value;
    }
    template <typename T>
    void writeMemory(uintptr_t address, const T &value) const
    {
        if (!WriteProcessMemory(handle, reinterpret_cast<LPVOID>(address), &value, sizeof(T), nullptr))
        {
            throw MemoryReaderException("Faild to write memory", GetLastError());
        }
    }

private:
    HANDLE handle;
};

class ProcessInfo
{
public:
    static DWORD getProcessIdByName(const std::wstring &processName)
    {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE)
        {
            throw MemoryReaderException("Failed to create process snapshot", GetLastError());
        }

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        DWORD pid = 0;
        if (Process32FirstW(hSnapshot, &pe32))
        {
            do
            {
                if (_wcsicmp(pe32.szExeFile, processName.c_str()) == 0)
                {
                    pid = pe32.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnapshot, &pe32));
        }

        CloseHandle(hSnapshot);

        if (pid == 0)
        {
            throw MemoryReaderException("Process not found : " + std::string(processName.begin(), processName.end()));
        }
        return pid;
    }

    static uintptr_t getModuleBaseAddress(DWORD pid, const std::wstring &moduleName)
    {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (hSnapshot == INVALID_HANDLE_VALUE)
        {
            throw MemoryReaderException("Faided to create module snapshot", GetLastError());
        }

        MODULEENTRY32W me32;
        me32.dwSize = sizeof(MODULEENTRY32W);

        uintptr_t baseAddress = 0;
        if (Module32FirstW(hSnapshot, &me32))
        {
            do
            {
                if (_wcsicmp(me32.szModule, moduleName.c_str()) == 0)
                {
                    baseAddress = reinterpret_cast<uintptr_t>(me32.modBaseAddr);
                    break;
                }
            } while (Module32NextW(hSnapshot, &me32));
        }
        CloseHandle(hSnapshot);

        if (baseAddress == 0)
        {
            std::wcout << "pid is " << pid;
            throw MemoryReaderException("Module not found: " + std::string(moduleName.begin(), moduleName.end()));
        }
        return baseAddress;
    }
};

class PointerChainResolver
{
public:
    PointerChainResolver(const ProcessHandle &processHandle, uintptr_t baseAddress) : processHandle(processHandle), currentAddress(baseAddress) {}

    PointerChainResolver &addOffset(uintptr_t offset)
    {
        offsets.push_back(offset);
        return *this;
    }
    PointerChainResolver &addOffsets(const std::vector<uintptr_t> &newOffsets)
    {
        offsets.insert(offsets.end(), newOffsets.begin(), newOffsets.end());
        return *this;
    }

    uintptr_t resolve()
    {
        // MODIFICATION: The last offset is not a pointer, but a final offset from the resolved address.
        // We need to resolve all but the last pointer.
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            // Read memory to get the next address in the chain
            if (currentAddress != 0) // Ensure we have a valid address to read from
            {
                 currentAddress = processHandle.readMemory<uintptr_t>(currentAddress);
            }
            else
            {
                throw MemoryReaderException("Null pointer in pointer chain", 0);
            }
            // Add the offset
            currentAddress += offsets[i];
        }
        return currentAddress;
    }

private:
    const ProcessHandle &processHandle;
    uintptr_t currentAddress;
    std::vector<uintptr_t> offsets;
};

class PlayerMonitor
{
public:
    PlayerMonitor(const ProcessHandle &processHandle, const std::wstring &name, uintptr_t baseAddress, const std::vector<uintptr_t> &healthOffsets)
        : processHandle(processHandle), playerName(name),
          healthAddress(0),
          currentHealth(0),
          healthChanged(false),
          running(false),
          baseAddress(baseAddress),
          healthOffsets(healthOffsets)
    {
        // Health address resolution is moved to the monitor thread to handle game restarts
    }

    virtual ~PlayerMonitor()
    {
        stop();
    }

    void start(int intervalMs = 100)
    {
        if (!running)
        {
            running = true;
            monitorThread = std::thread(&PlayerMonitor::monitorThreadFunc, this, intervalMs);
        }
    }

    void stop()
    {
        if (running)
        {
            running = false;
            if (monitorThread.joinable())
            {
                monitorThread.join();
            }
        }
    }

    int getCurrentHealth() const
    {
        return currentHealth;
    }

    virtual void onHealthChanged(int newHealth, int oldHealth)
    {
        // Default implementation does nothing
    }

protected:
    void monitorThreadFunc(int intervalMs)
    {
        int lastHealth = 0; // Initialize to 0

        while (running)
        {
            try
            {
                // Try to resolve the health address at the beginning of each loop
                // This makes it resilient to game restarts where the address might change
                if (healthAddress == 0) {
                    PointerChainResolver resolver(processHandle, this->baseAddress);
                    resolver.addOffsets(this->healthOffsets);
                    healthAddress = resolver.resolve();
                    // After resolving, read the initial health value
                    lastHealth = processHandle.readMemory<int>(healthAddress);
                    currentHealth = lastHealth;
                }

                int newHealth = processHandle.readMemory<int>(healthAddress);

                {
                    std::unique_lock<std::mutex> lock(mtx);
                    if (newHealth != lastHealth)
                    {
                        onHealthChanged(newHealth, lastHealth);
                        currentHealth = newHealth;
                        healthChanged = true;
                    }
                    lastHealth = newHealth;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            }
            catch (const MemoryReaderException &e)
            {
                // If memory reading fails (e.g., player not in match, game closed),
                // reset healthAddress so it will be re-resolved next iteration.
                healthAddress = 0;
                currentHealth = 0; // Reset health
                lastHealth = 0;
                // Log error to debug console if attached
                // std::wcerr << L"[" << playerName << L"] Error: " << e.what() << std::endl;
                // Wait before retrying
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    const ProcessHandle &processHandle;
    std::wstring playerName;
    uintptr_t healthAddress;
    uintptr_t baseAddress;
    std::vector<uintptr_t> healthOffsets;

    std::atomic<int> currentHealth;
    std::atomic<bool> healthChanged;

    std::atomic<bool> running;
    std::thread monitorThread;
    mutable std::mutex mtx;
};

class PlayerPositionDetector
{
public:
    PlayerPositionDetector(const ProcessHandle &processHandle,
                           uintptr_t netFightPlaceAddress,
                           uintptr_t localFightPlaceAddress)
        : processHandle(processHandle),
          netFightPlaceAddress(netFightPlaceAddress),
          localFightPlaceAddress(localFightPlaceAddress),
          currentPlayerPosition(0) {}

    void start(int intervalMs = 500)
    {
        if (!running)
        {
            running = true;
            detectorThread = std::thread(&PlayerPositionDetector::detectThreadFunc, this, intervalMs);
        }
    }

    void stop()
    {
        if (running)
        {
            running = false;
            if (detectorThread.joinable())
            {
                detectorThread.join();
            }
        }
    }

    PlayerPositions getPlayerPositions() const
    {
        std::lock_guard<std::mutex> lock(mtx);
        return currentPositions;
    }

private:
    void detectThreadFunc(int intervalMs)
    {
        while (running)
        {
            try
            {
                int netPosition = processHandle.readMemory<int>(netFightPlaceAddress);
                int localPosition = processHandle.readMemory<int>(localFightPlaceAddress);

                {
                    std::lock_guard<std::mutex> lock(mtx);
                    currentPositions.netPosition = netPosition;
                    currentPositions.localPosition = localPosition;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            }
            catch (const MemoryReaderException &e)
            {
                currentPlayerPosition = 0;
                // std::wcerr << L"[Position Detector] Error: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    const ProcessHandle &processHandle;
    uintptr_t netFightPlaceAddress;
    uintptr_t localFightPlaceAddress;

    std::atomic<bool> running{false};
    PlayerPositions currentPositions;
    mutable std::mutex mtx;
    std::thread detectorThread;
};

class CallbackPlayerMonitor : public PlayerMonitor
{
public:
    CallbackPlayerMonitor(const ProcessHandle &processHandle,
                          const std::wstring &name,
                          uintptr_t baseAddress,
                          const std::vector<uintptr_t> &healthOffsets,
                          int playerId)
        : PlayerMonitor(processHandle, name, baseAddress, healthOffsets),
          playerId(playerId) {}

protected:
    void onHealthChanged(int newHealth, int oldHealth) override
    {
        if (g_healthCallback)
        {
            g_healthCallback(playerId, newHealth, oldHealth);
        }
    }

private:
    int playerId;
};

class HealthMonitorController
{
public:
    HealthMonitorController(const std::wstring &processName,
                            const std::wstring &moduleName,
                            uintptr_t baseAddress1p,
                            const std::vector<uintptr_t> &healthOffsets1p,
                            uintptr_t baseAddress2p,
                            const std::vector<uintptr_t> &healthOffsets2p,
                            uintptr_t netFightPlaceAddress,
                            uintptr_t localFightPlaceAddress)
        : pid(ProcessInfo::getProcessIdByName(processName)),
          processHandle(pid, PROCESS_VM_READ),
          baseModuleAddress(ProcessInfo::getModuleBaseAddress(pid, moduleName)),
          player1Monitor(std::make_unique<CallbackPlayerMonitor>(
              processHandle,
              L"1P",
              baseModuleAddress + baseAddress1p,
              healthOffsets1p,
              1)),
          player2Monitor(std::make_unique<CallbackPlayerMonitor>(
              processHandle,
              L"2P",
              baseModuleAddress + baseAddress2p,
              healthOffsets2p,
              2)),
          playerPositionDetector(processHandle,
                                 baseModuleAddress + netFightPlaceAddress,
                                 baseModuleAddress + localFightPlaceAddress),
          running(false)
    {
        // 构造函数现在只负责初始化对象，不启动任何线程
    }

    ~HealthMonitorController()
    {
        stop();
    }

    // 【修改】方法名和返回值类型，以匹配底层调用
    PlayerPositions getPlayerPositions() const
    {
        return playerPositionDetector.getPlayerPositions();
    }

    int getPlayerHealth(int playerId) const
    {
        if (playerId == 1)
        {
            return player1Monitor->getCurrentHealth();
        }
        else if (playerId == 2)
        {
            return player2Monitor->getCurrentHealth();
        }
        return 0;
    }

    // 开始所有监控
    void start()
    {
        if (!running)
        {
            running = true;
            // 在这里启动所有监控线程，而不是在构造函数中
            player1Monitor->start();
            player2Monitor->start();
            playerPositionDetector.start();
        }
    }

    // 停止所有监控
    void stop()
    {
        if (running)
        {
            running = false;
        }
        // 停止玩家监控
        player1Monitor->stop();
        player2Monitor->stop();
        playerPositionDetector.stop();
    }

private:
    DWORD pid;
    ProcessHandle processHandle;
    uintptr_t baseModuleAddress;
    std::unique_ptr<CallbackPlayerMonitor> player1Monitor;
    std::unique_ptr<CallbackPlayerMonitor> player2Monitor;
    PlayerPositionDetector playerPositionDetector;

    std::atomic<bool> running;
    // **MODIFICATION**: Removed uiThread as it's no longer needed.
    // std::thread uiThread;
};

// --- C-style DLL Export Functions ---

// 初始化监控器
extern "C" MEMORYMONITOR_API bool InitializeMonitor(
    const wchar_t *processName,
    const wchar_t *moduleName,
    uintptr_t baseOffset1P,
    const uintptr_t *offsets1P, size_t offsetsCount1P,
    uintptr_t baseOffset2P,
    const uintptr_t *offsets2P, size_t offsetsCount2P,
    uintptr_t netFightPlaceOffset,
    uintptr_t localFightPlaceOffset,
    HealthChangedCallback callback)
{
    try
    {
        std::vector<uintptr_t> offsets1PVec(offsets1P, offsets1P + offsetsCount1P);
        std::vector<uintptr_t> offsets2PVec(offsets2P, offsets2P + offsetsCount2P);

        g_healthCallback = callback;

        g_monitorController = std::make_unique<HealthMonitorController>(
            processName,
            moduleName,
            baseOffset1P,
            offsets1PVec,
            baseOffset2P,
            offsets2PVec,
            netFightPlaceOffset,
            localFightPlaceOffset);

        return g_monitorController != nullptr;
    }
    catch (const std::exception &e)
    {

        OutputDebugStringA("InitializeMonitor error: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
    catch (...)
    {
        OutputDebugStringA("Unknown error in InitializeMonitor\n");
        return false;
    }
}

// 开始监控
extern "C" MEMORYMONITOR_API void StartMonitoring()
{
    if (g_monitorController)
    {
        g_monitorController->start();
    }
}

// 停止监控
extern "C" MEMORYMONITOR_API void StopMonitoring()
{
    if (g_monitorController)
    {
        // stop() 会停止所有线程
        g_monitorController->stop();
        // 释放控制器实例
        g_monitorController.reset();
        g_healthCallback = nullptr;
    }
}

// 【新增】导出函数，返回包含两个位置的结构体
extern "C" MEMORYMONITOR_API PlayerPositions GetPlayerPositions()
{
    if (g_monitorController)
    {
        return g_monitorController->getPlayerPositions();
    }
    // 如果控制器无效，返回一个清零的结构体
    return {0, 0};
}

// 获取玩家血量
extern "C" MEMORYMONITOR_API int GetPlayerHealth(int playerId)
{
    if (g_monitorController)
    {
        return g_monitorController->getPlayerHealth(playerId);
    }
    return 0;
}

// 如果定义了 TEST_MAIN，则编译测试用的 main 函数
#ifdef TEST_MAIN

// 测试用的回调函数
void __stdcall TestCallback(int playerId, int newHealth, int oldHealth)
{
    std::wcout << L"[Callback] Player " << playerId << L" health changed: "
               << oldHealth << L" -> " << newHealth << std::endl;
}

int main()
{
    try
    {
        const std::wstring processName = L"GGST-Win64-Shipping.exe";
        const std::wstring moduleName = L"GGST-Win64-Shipping.exe";

        const uintptr_t health_address_baseoffset_1p = 0x051B4158;
        const uintptr_t health_offsets_1p[] = {0x1C0, 0x28, 0x1220};

        const uintptr_t health_address_baseoffset_2p = 0x051B4158;
        const uintptr_t health_offsets_2p[] = {0x1C0, 0x1A0, 0x1220};

        const uintptr_t net_fight_place_address_offset = 0x4D383F4;
        const uintptr_t local_fight_place_address_offset = 0x4541FCC;

        std::wcout << L"Looking for process: " << processName << L"..." << std::endl;

        if (!InitializeMonitor(
                processName.c_str(),
                moduleName.c_str(),
                health_address_baseoffset_1p,
                health_offsets_1p, 3,
                health_address_baseoffset_2p,
                health_offsets_2p, 3,
                net_fight_place_address_offset,
                local_fight_place_address_offset,
                TestCallback))
        {
            std::wcerr << L"Failed to initialize monitor" << std::endl;
            return 1;
        }

        StartMonitoring();

        std::wcout << L"\nMonitoring started. Press Enter to exit..." << std::endl;
        std::cin.get();

        StopMonitoring();
        std::wcout << L"Monitoring stopped." << std::endl;
    }
    catch (const MemoryReaderException &e)
    {
        std::wcerr << L"Fatal error: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception &e)
    {
        std::wcerr << L"Unexpected error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

#endif
