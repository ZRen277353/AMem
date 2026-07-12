#pragma once

#include "Window.h"
#include "MemoryTypes.h"
#include "../mem/MemTypes.h"
#include "../utils/ScopedThread.h"
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <functional>

namespace Mem {
class IMemService;
}

class ScanWindow : public Window {
public:
    explicit ScanWindow(Mem::IMemService& memService);
    ~ScanWindow();
    unsigned int getWindowFlags() const override;

    // 扫描参数
    char valueBuf[128] = "100";
    char value2Buf[128] = "200";
    int scanType = 0;
    int valueType = 0;
    bool hexInput = false;

    // 内存类型选择
    uint32_t selectedMemoryTypes = MemoryType::All;
    bool showMemoryTypeModal = false;

    // 扫描结果
    std::vector<ScanResultItem> scanResults;
    std::vector<AddressListItem> addressList;
    std::vector<char> selectedScanResults;

    // 扫描状态
    std::atomic<bool> scanInProgress{false};
    std::atomic<bool> scanCancelled{false};
    float scanProgress = 0.0f;
    uint64_t scanMatchCount = 0;
    uint64_t scanTotalBytes = 0;
    uint64_t scanScannedBytes = 0;
    std::atomic<int> totalScanResults{0};
    std::atomic<bool> scanCompleted{false};
    std::atomic<bool> scanError{false};
    std::atomic<uint64_t> scanEpoch{0};

    // 结果分页
    std::atomic<int> resultOffset{0};
    int resultPageSize = 1000;

    // 地址列表自动刷新控制
    float addressListRefreshInterval = 1.0f;
    float timeSinceAddressListRefresh = 0.0f;
    bool autoRefreshAddressList = false;

    // 搜索结果自动刷新控制
    float scanResultsRefreshInterval = 5.0f;
    float timeSinceScanResultsRefresh = 0.0f;
    bool autoRefreshScanResults = false;

    // 异步刷新状态
    ScopedThread scanResultsRefreshThread;
    ScopedThread addressListRefreshThread;
    ScopedThread scanThread;
    std::mutex scanResultsMutex;
    std::mutex addressListMutex;
    std::mutex scanProgressMutex;

    // 刷新进度
    std::atomic<int> scanResultsRefreshProgress{0};
    std::atomic<int> scanResultsRefreshTotal{0};
    std::atomic<int> addressListRefreshProgress{0};
    std::atomic<int> addressListRefreshTotal{0};

    // 扫描结果显示状态
    bool showScanResultHexValues = false;
    bool sortScanResultsByValue = false;
    std::atomic<int> scanResultValueType{0};
    std::atomic<int> activeScanType{0};
    std::atomic<int> activeScanValueType{0};
    uint64_t resultContextMenuAddress = 0;
    std::string resultContextMenuValue;
    int resultContextMenuValueType = 0;
    bool scanResultsFirstRefreshLog = true;
    bool scanResultsLargePageWarningShown = false;
    uint64_t progressLastScannedBytes = 0;
    float progressLastUpdateTime = 0.0f;
    float progressStartTime = 0.0f;
    uint64_t observedProcessRevision = 0;

    void onDraw() override;
    void updateScanProgress(float progress, uint64_t matchCount, uint64_t scannedBytes, uint64_t totalBytes);

private:
    Mem::IMemService& memService_;
    Mem::CancellationToken scanCancellation_;
    void drawScanPanel();
    void drawResultsPanel();
    void drawAddressListPanel();
    void drawScanRangeSettings();
    void drawScanProgressBar();
    void drawMemoryTypeSelectionModal();
    void resetProcessState();

    // 扫描功能
    void performFirstScanAsync();
    void performNextScanAsync();
    void performNewScan();
    void requestScanCancellation();
    void loadScanResults();
    void loadScanResultsForRevision(uint64_t expectedProcessRevision);
    void refreshAddressValues();
    void refreshAddressValuesForRevision(uint64_t expectedProcessRevision);
    std::string readAddressValue(uint64_t address, int valueType);
    bool writeAddressValue(uint64_t address, int valueType, const std::string& value);
    void refreshScanResultsValues(int refreshValueType, bool warnLargePage);
    void refreshScanResultsValuesForRevision(int refreshValueType, bool warnLargePage, uint64_t expectedProcessRevision);
    void refreshScanResultsValuesAsync();
    void refreshAddressValuesAsync();
    void sortScanResultsForDisplay();

    // 扫描参数验证和处理
    bool validateScanParameters(bool isFirstScan);
    bool prepareValueBytes(std::vector<unsigned char>& valueBytes, bool isFirstScan);
    bool validateValueRange(const std::vector<unsigned char>& value1, const std::vector<unsigned char>& value2);

    // 数据类型转换
    std::vector<unsigned char> parseValueInput(const std::string& input, int type);
    std::string formatValueOutput(const std::vector<unsigned char>& data, int type);
    std::string formatValueOutput(const unsigned char* data, int type);
    std::string formatScanResultValue(uint64_t value, int type);
    std::string formatScanResultValueHex(uint64_t value, int type);
    uint32_t getValueTypeSize();
    const char* getScanTypeName(int scanType) const;
    const char* getValueTypeName(int valueType) const;
};
