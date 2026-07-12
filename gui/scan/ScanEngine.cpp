#include "../ScanWindow.h"
#include "../AppContext.h"
#include "../ColorScheme.h"
#include "../Gui.h"
#include "../../mem/IMemService.h"
#include "../../imgui/imgui.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <cerrno>
#include <cstdlib>

static bool IsScanProcessRevisionCurrent(uint64_t expectedProcessRevision)
{
    return AppContext::Get().processRevision.load(std::memory_order_acquire) == expectedProcessRevision;
}

static void ResetScanProgressForUi(ScanWindow* window)
{
    std::lock_guard<std::mutex> lock(window->scanProgressMutex);
    window->scanProgress = 0.0f;
    window->scanMatchCount = 0;
    window->scanScannedBytes = 0;
    window->scanTotalBytes = 0;
}

static Mem::ScanMode ToMemScanMode(int scanType)
{
    switch (scanType) {
        case UNKNOW_VAL:       return Mem::ScanMode::Unknown;
        case ACCURATE_VAL:     return Mem::ScanMode::Exact;
        case LARGER_THAN_VAL:  return Mem::ScanMode::Greater;
        case LESS_THAN_VAL:    return Mem::ScanMode::Less;
        case BETWEEN_VAL:      return Mem::ScanMode::Between;
        case ADD_UNKNOW_VAL:   return Mem::ScanMode::Increased;
        case ADD_ACCURATE_VAL: return Mem::ScanMode::IncreasedBy;
        case SUB_UNKNOW_VAL:   return Mem::ScanMode::Decreased;
        case SUB_ACCURATE_VAL: return Mem::ScanMode::DecreasedBy;
        case CHANGED_VAL:      return Mem::ScanMode::Changed;
        case UNCHANGED_VAL:    return Mem::ScanMode::Unchanged;
        default:               return Mem::ScanMode::Exact;
    }
}

static Mem::ScanDataType ToMemScanDataType(int valueType)
{
    switch (valueType) {
        case 0:  return Mem::ScanDataType::Byte;
        case 1:  return Mem::ScanDataType::Word;
        case 2:  return Mem::ScanDataType::Dword;
        case 3:  return Mem::ScanDataType::Qword;
        case 4:  return Mem::ScanDataType::Float;
        case 5:  return Mem::ScanDataType::Double;
        default: return Mem::ScanDataType::Dword;
    }
}

static Mem::ScanMemoryRegion ToMemScanMemoryRegion(uint32_t memoryTypes)
{
    if (memoryTypes == static_cast<uint32_t>(MemoryType::All)) {
        return Mem::ScanMemoryRegion::All;
    }
    return static_cast<Mem::ScanMemoryRegion>(static_cast<int32_t>(memoryTypes));
}

static void LogScanServiceError(const char* action, const Mem::Error& error)
{
    Gui::log("%s失败 [%s]: %s", action,
             Mem::errorCodeName(error.code), error.message.c_str());
}

static bool ParseUnsignedIntegerStrict(const std::string& input, int base, uint64_t maxValue, uint64_t& value)
{
    size_t begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || input[begin] == '-') {
        return false;
    }

    const char* str = input.c_str() + begin;
    char* end = nullptr;
    errno = 0;
    value = std::strtoull(str, &end, base);
    if (end == str || errno == ERANGE || value > maxValue) {
        return false;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }

    return *end == '\0';
}

static bool ParseFloatStrict(const std::string& input, float& value)
{
    const char* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtof(str, &end);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

static bool ParseDoubleStrict(const std::string& input, double& value)
{
    const char* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtod(str, &end);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

std::vector<unsigned char> ScanWindow::parseValueInput(const std::string& input, int type)
{
    std::vector<unsigned char> result;
    
    try {
        switch (type) {
            case 0: { // 1字节
                uint64_t parsed = 0;
                if (!ParseUnsignedIntegerStrict(input, hexInput ? 16 : 10, 0xFF, parsed)) {
                    result.clear();
                    break;
                }
                uint8_t value = static_cast<uint8_t>(parsed);
                result.resize(1);
                result[0] = value;
                break;
            }
            case 1: { // 2字节
                uint64_t parsed = 0;
                if (!ParseUnsignedIntegerStrict(input, hexInput ? 16 : 10, 0xFFFF, parsed)) {
                    result.clear();
                    break;
                }
                uint16_t value = static_cast<uint16_t>(parsed);
                result.resize(2);
                std::memcpy(result.data(), &value, 2);
                break;
            }
            case 2: { // 4字节
                uint64_t parsed = 0;
                if (!ParseUnsignedIntegerStrict(input, hexInput ? 16 : 10, 0xFFFFFFFFULL, parsed)) {
                    result.clear();
                    break;
                }
                uint32_t value = static_cast<uint32_t>(parsed);
                result.resize(4);
                std::memcpy(result.data(), &value, 4);
                break;
            }
            case 3: { // 8字节
                uint64_t value = 0;
                if (!ParseUnsignedIntegerStrict(input, hexInput ? 16 : 10, UINT64_MAX, value)) {
                    result.clear();
                    break;
                }
                result.resize(8);
                std::memcpy(result.data(), &value, 8);
                break;
            }
            case 4: { // 单精度浮点
                float value = 0.0f;
                if (!ParseFloatStrict(input, value)) {
                    result.clear();
                    break;
                }
                result.resize(4);
                std::memcpy(result.data(), &value, 4);
                break;
            }
            case 5: { // 双精度浮点
                double value = 0.0;
                if (!ParseDoubleStrict(input, value)) {
                    result.clear();
                    break;
                }
                result.resize(8);
                std::memcpy(result.data(), &value, 8);
                break;
            }
        }
    } catch (...) {
        result.clear();
    }
    
    return result;
}

std::string ScanWindow::formatValueOutput(const std::vector<unsigned char>& data, int type)
{
    if (data.empty()) return "无效";
    
    std::ostringstream oss;
    
    switch (type) {
        case 0: { // 1字节
            if (data.size() >= 1) {
                oss << (int)data[0];
            }
            break;
        }
        case 1: { // 2字节
            if (data.size() >= 2) {
                uint16_t value;
                std::memcpy(&value, data.data(), 2);
                oss << value;
            }
            break;
        }
        case 2: { // 4字节
            if (data.size() >= 4) {
                uint32_t value;
                std::memcpy(&value, data.data(), 4);
                oss << value;
            }
            break;
        }
        case 3: { // 8字节
            if (data.size() >= 8) {
                uint64_t value;
                std::memcpy(&value, data.data(), 8);
                oss << value;
            }
            break;
        }
        case 4: { // 单精度浮点
            if (data.size() >= 4) {
                float value;
                std::memcpy(&value, data.data(), 4);
                oss << std::fixed << std::setprecision(6) << value;
            }
            break;
        }
        case 5: { // 双精度浮点
            if (data.size() >= 8) {
                double value;
                std::memcpy(&value, data.data(), 8);
                oss << std::fixed << std::setprecision(10) << value;
            }
            break;
        }
    }
    
    return oss.str();
}

std::string ScanWindow::formatValueOutput(const unsigned char* data, int type)
{
    if (!data) return "无效";
    
    std::ostringstream oss;
    
    switch (type) {
        case 0: { // 1字节
            oss << (int)data[0];
            break;
        }
        case 1: { // 2字节
            uint16_t value;
            std::memcpy(&value, data, 2);
            oss << value;
            break;
        }
        case 2: { // 4字节
            uint32_t value;
            std::memcpy(&value, data, 4);
            oss << value;
            break;
        }
        case 3: { // 8字节
            uint64_t value;
            std::memcpy(&value, data, 8);
            oss << value;
            break;
        }
        case 4: { // 单精度浮点
            float value;
            std::memcpy(&value, data, 4);
            oss << std::fixed << std::setprecision(6) << value;
            break;
        }
        case 5: { // 双精度浮点
            double value;
            std::memcpy(&value, data, 8);
            oss << std::fixed << std::setprecision(10) << value;
            break;
        }
    }
    
    return oss.str();
}

// 异步版本的首次扫描
void ScanWindow::performFirstScanAsync()
{
    // 如果已经在扫描，不启动新的扫描
    if (scanInProgress) {
        Gui::log("扫描已在进行中，请等待完成");
        return;
    }
    
    // 等待之前的扫描线程完成
    scanThread.stop();
    scanResultsRefreshThread.stop();
    addressListRefreshThread.stop();

    // 在主线程中进行参数验证和准备
    if (!validateScanParameters(true)) {
        return;
    }
    
    std::vector<unsigned char> valueBytes;
    if (!prepareValueBytes(valueBytes, true)) {
        return;
    }
    
    const int currentScanType = scanType;
    const int currentValueType = valueType;
    const uint32_t memoryTypeFlags = selectedMemoryTypes;
    Mem::ScanStartRequest request;
    request.kind = currentScanType == UNKNOW_VAL
        ? Mem::ScanStartKind::Unknown
        : Mem::ScanStartKind::Value;
    request.dataType = ToMemScanDataType(currentValueType);
    request.mode = ToMemScanMode(currentScanType);
    request.memoryRegion = ToMemScanMemoryRegion(memoryTypeFlags);
    request.value = std::move(valueBytes);

    Mem::OperationContext context = memService_.captureContext(true);
    if (!context.target) {
        Gui::log("首次扫描失败: 当前进程目标不可用");
        return;
    }
    const uint64_t expectedProcessRevision = context.target->processRevision;
    scanCancellation_ = std::make_shared<std::atomic<bool>>(false);
    context.cancellation = scanCancellation_;
    
    // 重置进度状态
    scanInProgress = true;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    activeScanType = currentScanType;
    activeScanValueType = currentValueType;
    scanResultValueType = currentValueType;
    ResetScanProgressForUi(this);
    
    Gui::log("开始首次扫描（异步）...");
    Gui::log("扫描类型: %s", getScanTypeName(currentScanType));
    Gui::log("数值类型: %s", getValueTypeName(currentValueType));
    Gui::log("内存类型标志: 0x%X", memoryTypeFlags);
    
    scanThread.launch([this, context, request, currentValueType, expectedProcessRevision](const std::atomic<bool>& /*cancel*/) {
        const auto response = memService_.startScan(
            context, request,
            [this](const Mem::ScanProgressUpdate& update) {
                updateScanProgress(update.progress, update.matchCount,
                                   update.scannedBytes, update.totalBytes);
            });
        if (!IsScanProcessRevisionCurrent(expectedProcessRevision)) {
            scanInProgress = false;
            return;
        }

        if (!response.ok()) {
            scanError = true;
            scanCompleted = false;
            scanEpoch = 0;
            totalScanResults = 0;
            resultOffset = 0;
            {
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
            }
            LogScanServiceError("首次扫描", response.error());
            scanInProgress = false;
            return;
        }

        const auto& summary = response.value();
        scanEpoch = summary.session.epoch;
        totalScanResults = static_cast<int>(summary.session.resultCount);
        scanResultValueType = currentValueType;
        resultOffset = 0;
        scanCancelled = summary.completedAfterCancelRequest;
        scanCompleted = true;
        if (summary.completedAfterCancelRequest) {
            Gui::log("停止请求发出后扫描仍返回了已确认结果，共 %d 个",
                     totalScanResults.load());
        } else {
            Gui::log("首次扫描完成，找到 %d 个结果", totalScanResults.load());
        }

        if (totalScanResults.load() > 0) {
            loadScanResultsForRevision(expectedProcessRevision);
        } else {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            scanResults.clear();
            selectedScanResults.clear();
        }
        scanInProgress = false;
    });
}

// 异步版本的再次扫描
void ScanWindow::performNextScanAsync()
{
    // 如果已经在扫描，不启动新的扫描
    if (scanInProgress) {
        Gui::log("扫描已在进行中，请等待完成");
        return;
    }
    
    // 等待之前的扫描线程完成
    scanThread.stop();
    scanResultsRefreshThread.stop();
    addressListRefreshThread.stop();

    // 在主线程中进行参数验证和准备
    if (!validateScanParameters(false)) {
        return;
    }
    
    std::vector<unsigned char> valueBytes;
    if (!prepareValueBytes(valueBytes, false)) {
        return;
    }
    
    const int currentScanType = scanType;
    const int currentValueType = valueType;
    Mem::ScanRefineRequest request;
    request.expectedEpoch = scanEpoch.load();
    request.dataType = ToMemScanDataType(currentValueType);
    request.mode = ToMemScanMode(currentScanType);
    request.value = std::move(valueBytes);

    Mem::OperationContext context = memService_.captureContext(true);
    if (!context.target) {
        Gui::log("再次扫描失败: 当前进程目标不可用");
        return;
    }
    const uint64_t expectedProcessRevision = context.target->processRevision;
    scanCancellation_ = std::make_shared<std::atomic<bool>>(false);
    context.cancellation = scanCancellation_;
    
    // 重置进度状态
    scanInProgress = true;
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    activeScanType = currentScanType;
    activeScanValueType = currentValueType;
    scanResultValueType = currentValueType;
    ResetScanProgressForUi(this);
    
    Gui::log("开始再次扫描（异步）...");
    Gui::log("扫描类型: %s", getScanTypeName(currentScanType));
    Gui::log("数值类型: %s", getValueTypeName(currentValueType));
    Gui::log("当前结果数: %d", totalScanResults.load());
    
    scanThread.launch([this, context, request, currentValueType, expectedProcessRevision](const std::atomic<bool>& /*cancel*/) {
        const auto response = memService_.refineScan(
            context, request,
            [this](const Mem::ScanProgressUpdate& update) {
                updateScanProgress(update.progress, update.matchCount,
                                   update.scannedBytes, update.totalBytes);
            });
        if (!IsScanProcessRevisionCurrent(expectedProcessRevision)) {
            scanInProgress = false;
            return;
        }

        if (!response.ok()) {
            scanError = true;
            scanCompleted = false;
            if (response.error().code != Mem::ErrorCode::CancelRequested &&
                response.error().code != Mem::ErrorCode::InvalidArgument) {
                scanEpoch = 0;
                totalScanResults = 0;
                resultOffset = 0;
                std::lock_guard<std::mutex> lock(scanResultsMutex);
                scanResults.clear();
                selectedScanResults.clear();
            }
            LogScanServiceError("再次扫描", response.error());
            scanInProgress = false;
            return;
        }

        const auto& summary = response.value();
        scanEpoch = summary.session.epoch;
        totalScanResults = static_cast<int>(summary.session.resultCount);
        scanResultValueType = currentValueType;
        resultOffset = 0;
        scanCancelled = summary.completedAfterCancelRequest;
        scanCompleted = true;
        if (summary.completedAfterCancelRequest) {
            Gui::log("停止请求发出后再次扫描仍返回了已确认结果，共 %d 个",
                     totalScanResults.load());
        } else {
            Gui::log("再次扫描完成，找到 %d 个结果", totalScanResults.load());
        }

        if (totalScanResults.load() > 0) {
            loadScanResultsForRevision(expectedProcessRevision);
        } else {
            std::lock_guard<std::mutex> lock(scanResultsMutex);
            scanResults.clear();
            selectedScanResults.clear();
        }
        scanInProgress = false;
    });
}

void ScanWindow::performNewScan()
{
    scanResultsRefreshThread.stop();
    addressListRefreshThread.stop();

    Mem::ScanClearRequest request;
    request.expectedEpoch = scanEpoch.load();
    const auto response = memService_.clearScan(
        memService_.captureContext(true), request);
    if (!response.ok()) {
        LogScanServiceError("清除扫描结果", response.error());
    } else {
        Gui::log("已清除扫描结果");
    }
    
    {
        std::lock_guard<std::mutex> lock(scanResultsMutex);
        scanResults.clear();
        selectedScanResults.clear();
    }
    
    totalScanResults = 0;
    resultOffset = 0;
    scanEpoch = 0;
    scanResultValueType = valueType;
    
    // 重置进度状态
    ResetScanProgressForUi(this);
    scanCompleted = false;
    scanError = false;
    scanCancelled = false;
    
    Gui::log("本地扫描状态已重置，可以开始新扫描");
}


bool ScanWindow::validateScanParameters(bool isFirstScan)
{
    // 检查进程是否选择
    if (!AppContext::Get().hasProcess()) {
        Gui::log("请先选择进程");
        return false;
    }
    
    // 检查内存类型是否选择
    if (selectedMemoryTypes == 0) {
        Gui::log("请先选择内存类型");
        return false;
    }
    
    // 检查是否正在扫描
    if (scanInProgress) {
        Gui::log("扫描正在进行中，请等待完成");
        return false;
    }
    
    // 对于再次扫描，检查是否有之前的结果
    if (!isFirstScan && totalScanResults.load() == 0) {
        Gui::log("请先执行首次扫描");
        return false;
    }

    if (!isFirstScan && scanType == UNKNOW_VAL) {
        Gui::log("未知初始值只能用于首次扫描，请选择变动、未变动、增加或减少等再次扫描类型");
        return false;
    }

    if (!isFirstScan && valueType != scanResultValueType.load()) {
        Gui::log("再次扫描的数值类型必须与当前结果一致 (%s)，请切回原类型或新建扫描",
                 getValueTypeName(scanResultValueType.load()));
        return false;
    }
    
    // 验证扫描类型的有效性
    if (isFirstScan) {
        if (scanType == ADD_UNKNOW_VAL || scanType == ADD_ACCURATE_VAL ||
            scanType == SUB_UNKNOW_VAL || scanType == SUB_ACCURATE_VAL ||
            scanType == CHANGED_VAL || scanType == UNCHANGED_VAL) {
            Gui::log("错误: '%s' 只能在再次扫描中使用，请先进行未知初始值或精确数值扫描", getScanTypeName(scanType));
            return false;
        }
    }
    
    return true;
}

bool ScanWindow::prepareValueBytes(std::vector<unsigned char>& valueBytes, bool isFirstScan)
{
    // 检查是否需要输入值的扫描类型
    bool needValue1 = !(scanType == UNKNOW_VAL || scanType == ADD_UNKNOW_VAL || 
                       scanType == SUB_UNKNOW_VAL || scanType == CHANGED_VAL || 
                       scanType == UNCHANGED_VAL);
    bool needValue2 = (scanType == BETWEEN_VAL);
    
    valueBytes.clear();
    
    // 处理第一个值
    if (needValue1) {
        // 检查输入是否为空
        if (strlen(valueBuf) == 0) {
            Gui::log("请输入数值");
            return false;
        }
        
        std::vector<unsigned char> value1Bytes = parseValueInput(valueBuf, valueType);
        if (value1Bytes.empty()) {
            Gui::log("无效的数值输入: %s", valueBuf);
            return false;
        }
        
        valueBytes = value1Bytes;
    } else {
        // 对于不需要值的扫描类型，创建一个占位字节数组
        valueBytes.resize(getValueTypeSize());
        // 填充为0
        std::fill(valueBytes.begin(), valueBytes.end(), 0);
    }
    
    // 处理第二个值（范围扫描）
    if (needValue2) {
        if (strlen(value2Buf) == 0) {
            Gui::log("范围扫描需要输入最大值");
            return false;
        }
        
        std::vector<unsigned char> value2Bytes = parseValueInput(value2Buf, valueType);
        if (value2Bytes.empty()) {
            Gui::log("无效的最大值输入: %s", value2Buf);
            return false;
        }
        
        // 验证范围是否合理
        if (!validateValueRange(valueBytes, value2Bytes)) {
            return false;
        }
        
        // 对于范围扫描，将两个值合并
        valueBytes.insert(valueBytes.end(), value2Bytes.begin(), value2Bytes.end());
    }
    
    return true;
}

bool ScanWindow::validateValueRange(const std::vector<unsigned char>& value1, const std::vector<unsigned char>& value2)
{
    if (value1.size() != value2.size()) {
        Gui::log("内部错误: 数值大小不匹配");
        return false;
    }
    
    bool isValid = false;
    switch (valueType) {
        case 0: { // 1字节
            uint8_t val1 = value1[0];
            uint8_t val2 = value2[0];
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%u)必须小于等于最大值(%u)", val1, val2);
            }
            break;
        }
        case 1: { // 2字节
            uint16_t val1, val2;
            std::memcpy(&val1, value1.data(), 2);
            std::memcpy(&val2, value2.data(), 2);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%u)必须小于等于最大值(%u)", val1, val2);
            }
            break;
        }
        case 2: { // 4字节
            uint32_t val1, val2;
            std::memcpy(&val1, value1.data(), 4);
            std::memcpy(&val2, value2.data(), 4);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%u)必须小于等于最大值(%u)", val1, val2);
            }
            break;
        }
        case 3: { // 8字节
            uint64_t val1, val2;
            std::memcpy(&val1, value1.data(), 8);
            std::memcpy(&val2, value2.data(), 8);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%llu)必须小于等于最大值(%llu)", val1, val2);
            }
            break;
        }
        case 4: { // 单精度浮点
            float val1, val2;
            std::memcpy(&val1, value1.data(), 4);
            std::memcpy(&val2, value2.data(), 4);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%.6f)必须小于等于最大值(%.6f)", val1, val2);
            }
            break;
        }
        case 5: { // 双精度浮点
            double val1, val2;
            std::memcpy(&val1, value1.data(), 8);
            std::memcpy(&val2, value2.data(), 8);
            isValid = val1 <= val2;
            if (!isValid) {
                Gui::log("错误: 最小值(%.10f)必须小于等于最大值(%.10f)", val1, val2);
            }
            break;
        }
        default:
            Gui::log("错误: 不支持的数值类型");
            return false;
    }
    
    return isValid;
}

std::string ScanWindow::formatScanResultValue(uint64_t value, int type)
{
    std::ostringstream oss;

    switch (type) {
        case 0: { // 1字节
            uint8_t byteValue = static_cast<uint8_t>(value & 0xFF);
            oss << static_cast<int>(byteValue);
            break;
        }
        case 1: { // 2字节
            uint16_t wordValue = static_cast<uint16_t>(value & 0xFFFF);
            oss << wordValue;
            break;
        }
        case 2: { // 4字节
            uint32_t dwordValue = static_cast<uint32_t>(value & 0xFFFFFFFF);
            oss << dwordValue;
            break;
        }
        case 3: { // 8字节
            oss << value;
            break;
        }
        case 4: { // 单精度浮点
            uint32_t dwordValue = static_cast<uint32_t>(value & 0xFFFFFFFF);
            float floatValue;
            std::memcpy(&floatValue, &dwordValue, sizeof(float));

            if (std::isfinite(floatValue)) {
                if (std::abs(floatValue) >= 1e6 || (std::abs(floatValue) < 1e-3 && floatValue != 0.0f)) {
                    oss << std::scientific << std::setprecision(3) << floatValue;
                } else {
                    oss << std::fixed << std::setprecision(6) << floatValue;
                }
            } else if (std::isnan(floatValue)) {
                oss << "NaN";
            } else if (std::isinf(floatValue)) {
                oss << (floatValue > 0 ? "+∞" : "-∞");
            } else {
                oss << "无效浮点数";
            }
            break;
        }
        case 5: { // 双精度浮点
            double doubleValue;
            std::memcpy(&doubleValue, &value, sizeof(double));

            if (std::isfinite(doubleValue)) {
                if (std::abs(doubleValue) >= 1e12 || (std::abs(doubleValue) < 1e-6 && doubleValue != 0.0)) {
                    oss << std::scientific << std::setprecision(6) << doubleValue;
                } else {
                    oss << std::fixed << std::setprecision(10) << doubleValue;
                }
            } else if (std::isnan(doubleValue)) {
                oss << "NaN";
            } else if (std::isinf(doubleValue)) {
                oss << (doubleValue > 0 ? "+∞" : "-∞");
            } else {
                oss << "无效浮点数";
            }
            break;
        }
        default:
            oss << "未知类型: " << value;
            break;
    }

    return oss.str();
}

std::string ScanWindow::formatScanResultValueHex(uint64_t value, int type)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase;

    switch (type) {
        case 0: { // 1字节
            uint8_t byteValue = static_cast<uint8_t>(value & 0xFF);
            oss << "0x" << std::setfill('0') << std::setw(2) << static_cast<int>(byteValue);
            break;
        }
        case 1: { // 2字节
            uint16_t wordValue = static_cast<uint16_t>(value & 0xFFFF);
            oss << "0x" << std::setfill('0') << std::setw(4) << wordValue;
            break;
        }
        case 2: { // 4字节
            uint32_t dwordValue = static_cast<uint32_t>(value & 0xFFFFFFFF);
            oss << "0x" << std::setfill('0') << std::setw(8) << dwordValue;
            break;
        }
        case 3: { // 8字节
            oss << "0x" << std::setfill('0') << std::setw(16) << value;
            break;
        }
        default:
            oss << "0x" << value;
            break;
    }

    return oss.str();
}

uint32_t ScanWindow::getValueTypeSize()
{
    switch (valueType) {
        case 0: return 1;          // 1字节
        case 1: return 2;          // 2字节
        case 2: return 4;          // 4字节
        case 3: return 8;          // 8字节
        case 4: return 4;          // 单精度浮点 4字节
        case 5: return 8;          // 双精度浮点 8字节
        default: return 4;
    }
}

const char* ScanWindow::getScanTypeName(int scanType) const
{
    switch (scanType) {
        case UNKNOW_VAL: return "未知初始值";
        case ACCURATE_VAL: return "精确数值";
        case LARGER_THAN_VAL: return "大于数值";
        case LESS_THAN_VAL: return "小于数值";
        case BETWEEN_VAL: return "值在范围内";
        case ADD_UNKNOW_VAL: return "值增加了未知值";
        case ADD_ACCURATE_VAL: return "值增加了精确值";
        case SUB_UNKNOW_VAL: return "值减少了未知值";
        case SUB_ACCURATE_VAL: return "值减少了精确值";
        case CHANGED_VAL: return "变动的数值";
        case UNCHANGED_VAL: return "未变动的数值";
        default: return "未知扫描类型";
    }
}

const char* ScanWindow::getValueTypeName(int valueType) const
{
    switch (valueType) {
        case 0: return "1字节 (BYTE)";
        case 1: return "2字节 (WORD)";
        case 2: return "4字节 (DWORD)";
        case 3: return "8字节 (QWORD)";
        case 4: return "单精度浮点";
        case 5: return "双精度浮点";
        default: return "未知数值类型";
    }
}
