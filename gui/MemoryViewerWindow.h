#pragma once

#include "Window.h"
#include "DisassemblyHelper.h"
#include "../mem/MemTypes.h"
#include "../mem/MemResult.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace Mem {
class IMemService;
}

// 数据结构字段类型枚举
enum class FieldType {
    BYTE = 0,
    WORD,
    DWORD,
    QWORD,
    FLOAT,
    DOUBLE,
    POINTER,
    STRING,
    STRING_UTF8,
    STRING_UTF16,
    ARRAY,
    STRUCT
};

// 内存监控项（类似CE的地址列表）
struct MemoryWatchItem {
    std::string description;        // 描述
    uint64_t address;               // 地址
    FieldType type;                 // 数据类型
    bool enabled;                   // 是否启用
    bool frozen;                    // 是否冻结
    uint8_t frozenData[8];          // 冻结值（原始字节）
    uint8_t frozenDataSize;         // 冻结值字节数（0 表示未设置，最大 8）
    uint64_t frozenAddress = 0;     // Address currently registered with server-side freeze.
    std::vector<uint64_t> offsets;  // 偏移链
    std::string moduleName;         // 模块名
    uint64_t moduleOffset;          // 模块偏移
    bool isPointer;                 // 是否是指针
    int arrayLength;                // 如果是数组，数组长度
    std::string cachedValue;        // 缓存的当前值，避免每帧读取
    char editAddressBuffer[32] = "";
    bool addressEditActive = false;
    char editValueBuffer[256] = "";
    bool valueEditActive = false;

    MemoryWatchItem() : address(0), type(FieldType::DWORD), enabled(true),
                        frozen(false), frozenDataSize(0), moduleOffset(0),
                        isPointer(false), arrayLength(1), cachedValue("N/A") {
        memset(frozenData, 0, sizeof(frozenData));
    }
};

// 数据结构字段定义
struct StructField {
    std::string name;
    FieldType type;
    int offset;
    int size;
    int arrayCount = 1;  // 数组元素个数，默认1表示非数组
    std::string description;
    bool isPointer = false;
    std::string structTypeName;  // 如果是结构体类型，存储结构体类型名
    
    StructField() = default;
    StructField(const std::string& n, FieldType t, int off, int sz) 
        : name(n), type(t), offset(off), size(sz) {}
};

// 数据结构定义
struct StructDefinition {
    std::string name;
    std::vector<StructField> fields;
    int totalSize = 0;
    
    void calculateSize() {
        totalSize = 0;
        for (const auto& field : fields) {
            if (field.offset < 0 || field.size <= 0 || field.arrayCount <= 0) {
                continue;
            }
            const int64_t fieldBytes =
                static_cast<int64_t>(field.size) * static_cast<int64_t>(field.arrayCount);
            if (fieldBytes <= 0 ||
                fieldBytes > (std::numeric_limits<int>::max)() - field.offset) {
                totalSize = (std::numeric_limits<int>::max)();
                continue;
            }
            const int fieldEnd = field.offset + static_cast<int>(fieldBytes);
            if (fieldEnd > totalSize) {
                totalSize = fieldEnd;
            }
        }
    }
};

// Dissector 树节点（CE 风格，支持指针展开）
struct DissectNode {
    int offset;              // 相对父级基址的字节偏移
    FieldType type;          // 当前解释类型
    std::string name;        // 用户命名
    std::string cachedValue; // 缓存的显示值
    std::string description; // 用户备注
    char editValueBuffer[256] = "";
    bool valueEditActive = false;
    int storedSize = 0;      // 该行占用字节数（STRING 等变长类型用）

    // 树结构
    bool expanded = false;               // 指针是否展开
    uint64_t pointerTarget = 0;          // 指针目标地址
    std::vector<unsigned char> childBuffer; // 子节点内存缓冲
    std::vector<DissectNode> children;   // 子节点
    int depth = 0;                       // 嵌套深度

    int getSize() const {
        switch (type) {
            case FieldType::BYTE: return 1;
            case FieldType::WORD: return 2;
            case FieldType::DWORD: return 4;
            case FieldType::QWORD: return 8;
            case FieldType::FLOAT: return 4;
            case FieldType::DOUBLE: return 8;
            case FieldType::POINTER: return 8;
            case FieldType::STRING:
            case FieldType::STRING_UTF8:
            case FieldType::STRING_UTF16:
                return storedSize > 0 ? storedSize : 1;
            default: return 4;
        }
    }
};

// 显示格式枚举（移到类外部）
enum class DisplayFormat {
    Hex_Byte = 0,      // 单字节十六进制
    Hex_2Bytes,        // 2字节十六进制
    Hex_4Bytes,        // 4字节十六进制
    Hex_8Bytes,        // 8字节十六进制
    Dec_Byte,          // 单字节十进制
    Dec_2Bytes,        // 2字节十进制
    Dec_4Bytes,        // 4字节十进制
    Dec_8Bytes,        // 8字节十进制
    Float_4Bytes,      // 4字节浮点
    Double_8Bytes,     // 8字节双精度
    Binary_Byte        // 二进制
};

// ASCII显示模式枚举
enum class AsciiDisplayMode {
    ASCII = 0,         // ASCII文本
    UTF8,              // UTF-8文本
    UTF16              // UTF-16文本
};

class MemoryViewerWindow : public Window {
public:
    explicit MemoryViewerWindow(Mem::IMemService& memService);
    ~MemoryViewerWindow();

    void onDraw() override;
    unsigned int getWindowFlags() const override;

    void jumpToAddress(uint64_t address);

private:
    Mem::IMemService& memService_;
    bool readTargetMemory(
        uint64_t address, uint32_t size,
        std::vector<unsigned char>& bytes,
        Mem::MemoryReadChannel channel = Mem::MemoryReadChannel::Foreground,
        Mem::Error* error = nullptr);
    bool writeTargetMemory(
        uint64_t address, const std::vector<unsigned char>& bytes,
        Mem::Error* error = nullptr);
    bool addFrozenValue(
        uint64_t address, const unsigned char* bytes, size_t size,
        Mem::Error* error = nullptr);
    bool updateFrozenValue(
        uint64_t address, const unsigned char* bytes, size_t size,
        Mem::Error* error = nullptr);
    bool removeFrozenValue(uint64_t address, Mem::Error* error = nullptr);
    bool readDissectMemory(
        uint64_t address, int size, std::vector<unsigned char>& bytes);
    bool resolveWatchItemAddress(
        const MemoryWatchItem& item, uint64_t& address);
    void clearWatchItemFreeze(MemoryWatchItem& item);
    void clearWatchItemFreezes(std::vector<MemoryWatchItem>& items);
    int navSubscriptionId = 0;  // EventBus 订阅 ID
    uint64_t observedProcessRevision = 0;
    static bool parseAddressExpression(const char* expr, uint64_t& result);
    void resetProcessState();
    void drawMemoryViewerPanel();
    void drawStructAnalyzerPanel();
    void drawDissectorTable();
    void drawDataInspector();  // 新增：数据类型解析面板
    void drawMemoryHexEditor(); // 新增：十六进制编辑器
    void drawAddressList();     // 新增：地址列表（类似CE）
    void drawAddItemDialog();   // 新增：添加监控项对话框
    void drawDisassemblyPanel(); // 新增：反汇编查看器面板
    
    // 辅助方法
    const char* getFieldTypeName(FieldType type);
    int getFieldTypeSize(FieldType type);
    std::string readFieldValue(const std::vector<unsigned char>& data, const StructField& field, uint64_t baseAddr);
    std::string readSingleFieldValue(const std::vector<unsigned char>& data, FieldType type, int offset, int size);
    void saveStructDefinitions();
    void loadStructDefinitions();
    void refreshMemory();  // 新增：刷新内存
    void writeMemoryByte(uint64_t address, unsigned char value);  // 新增：写入单字节
    void addToHistory(uint64_t address);  // 新增：添加到历史记录
    void formatValueString(char* output, size_t outputSize, size_t bufferIndex, DisplayFormat format);  // 新增：格式化值字符串
    std::string readUTF8String(const std::vector<unsigned char>& data, size_t offset, size_t maxLength);  // 读取UTF-8字符串
    std::string readUTF16String(const std::vector<unsigned char>& data, size_t offset, size_t maxLength);  // 读取UTF-16字符串
    std::string utf16ToUtf8(const uint16_t* utf16Str, size_t length);  // UTF-16转UTF-8
    
    // 监控项相关
    std::string readWatchItemValue(MemoryWatchItem& item);  // 读取监控项的值
    bool writeWatchItemValue(MemoryWatchItem& item, const std::string& value);  // 写入监控项的值
    void updateWatchItems();  // 更新所有监控项
    void saveWatchList();     // 保存监控列表
    void loadWatchList();     // 加载监控列表
    
    // 结构体分析相关
    void addStructToWatchList(const StructDefinition& structDef);
    bool writeStructFieldValue(int fieldIndex, const std::string& value);

    // Dissector 相关（树形结构）
    void drawDissectorToolbar();
    void drawNodeEditPopup();
    void regenerateDissectNodes();
    void refreshDissectValues();
    void refreshNodeValues(std::vector<DissectNode>& nodes, const std::vector<unsigned char>& buffer, uint64_t baseAddr);
    void onDissectNodeTypeChanged(std::vector<DissectNode>& nodes, int nodeIndex, FieldType newType, int stringSize = 32);
    void expandPointerNode(DissectNode& node, uint64_t baseAddr);
    void collapsePointerNode(DissectNode& node);
    bool writeDissectNodeValue(DissectNode& node, uint64_t baseAddr, const std::string& valueStr);
    void saveDissectAsTemplate(const std::string& name);
    void loadTemplateIntoDissector(int index);
    void autoAnalyzeNodes(const std::vector<unsigned char>& data);
    DissectNode* resolveNodeByPath(const std::vector<int>& path);

    // drawNodeRow 延迟操作参数结构
    struct DissectDeferredOps {
        int pendingTypeChangeFlat = -1;
        FieldType pendingNewType = FieldType::DWORD;
        std::vector<int> pendingTypeChangePath;
        int pendingExpandFlat = -1;
        std::vector<int> pendingExpandPath;
        int pendingWriteFlat = -1;
        std::string pendingWriteValue;
        std::vector<int> pendingWritePath;
        // 偏移调整
        int pendingOffsetChangeFlat = -1;
        int pendingNewOffset = 0;
        std::vector<int> pendingOffsetChangePath;
    };
    void drawNodeRow(DissectNode& node, uint64_t baseAddr, int& flatIndex,
                     std::vector<int>& path, DissectDeferredOps& ops);
    void onDissectNodeOffsetChanged(std::vector<DissectNode>& nodes, int nodeIndex, int newOffset);
    
    // 反汇编相关
    std::string formatAddressWithOffset(uint64_t address);  // 格式化地址显示：地址[偏移量]
    
    // 内存查看器状态
    uint64_t viewAddress = 0;
    uint64_t pageBaseAddress = 0;  // 页首地址（4K对齐）
    uint64_t targetAddress = 0;    // 目标地址（用于滚动聚焦）
    char hexAddressInputBuf[64] = "";
    uint64_t lastHexAddressInputTarget = 0;
    bool hexAutoScrolling = false;
    int bytesPerRow = 16;
    int viewSize = 4096;  // 默认一页大小
    int pageSize = 4096;   // 页大小（4KB）
    bool scrollToTarget = false;  // 是否需要滚动到目标地址
    std::vector<unsigned char> buffer;
    
    // 新增：编辑相关
    bool editMode = false;
    int editingRow = -1;
    int editingCol = -1;
    char editBuffer[3] = "";
    
    // 新增：选中位置（用于数据解析）
    int selectedByteOffset = -1;
    uint64_t selectedByteAddress = 0;
    
    // 新增：导航历史
    std::vector<uint64_t> addressHistory;
    int historyIndex = -1;
    
    // 新增：自动刷新
    bool autoRefresh = false;
    float refreshInterval = 2.0f;  // 秒（增加默认间隔，减少性能开销）
    float timeSinceRefresh = 0.0f;
    
    // 新增：显示格式
    int displayFormat = 0;  // 显示格式索引
    int asciiDisplayMode = 0;  // ASCII显示模式索引

    // 模块 + 偏移链
    char moduleNameBuf[256] = "";
    char baseOffsetBuf[32] = "0";
    char offsetsBuf[256] = "";
    bool derefFinal = true;
    
    // 动态偏移链
    std::vector<uint64_t> offsetChain;  // 偏移链数组
    char newOffsetBuf[32] = "0";  // 新增偏移输入框
    int selectedOffsetIndex = -1;  // 选中的偏移索引
    
    // 数据结构分析器状态
    bool showStructAnalyzer = false;
    std::vector<StructDefinition> structDefinitions;
    int selectedStructIndex = -1;
    uint64_t structBaseAddress = 0;
    std::vector<unsigned char> structBuffer;
    char newStructName[128] = "";

    // Dissector 状态
    std::vector<DissectNode> dissectNodes;
    int dissectDefaultSize = 4;          // 默认元素大小 1/2/4/8
    int dissectTotalSize = 256;          // 显示区域总字节数
    bool dissectAutoRefresh = false;
    float dissectRefreshInterval = 1.0f;
    float timeSinceDissectRefresh = 0.0f;

    // 指针跟踪历史
    std::vector<uint64_t> dissectAddrHistory;
    int dissectHistoryIdx = -1;

    // 保存模板对话框
    bool showSaveTemplateDialog = false;
    char saveTemplateName[128] = "";

    // 节点编辑弹窗
    bool showNodeEditPopup = false;
    std::vector<int> dissectEditPath;
    char editNodeName[128] = "";
    char editNodeDesc[256] = "";
    int editNodeTypeIdx = 0;
    int editNodeStringSize = 32;
    int editNodeOffset = 0;  // 编辑弹窗中的偏移值
    
    // 地址列表（监控项）
    std::vector<MemoryWatchItem> watchItems;
    std::vector<std::string> watchLastValues;
    int selectedWatchIndex = -1;
    bool showAddItemDialog = false;
    
    // 添加项对话框状态
    char newItemDesc[256] = "";
    char newItemAddress[32] = "";
    int newItemType = 0;  // FieldType索引
    bool newItemIsPointer = false;
    char newItemOffsets[256] = "";
    float watchUpdateInterval = 1.0f;  // 监控更新间隔（增加默认间隔，减少内存读写频率）
    float timeSinceWatchUpdate = 0.0f;
    
    // 反汇编查看器状态
    std::unique_ptr<DisassemblyHelper> disassemblyHelper;  // 反汇编引擎
    bool disassemblyInitialized = false;  // 反汇编引擎是否已初始化
    char disassemblyAddressBuf[64] = "";  // 反汇编地址输入框
    uint64_t disassemblyAddress = 0;  // 当前反汇编地址（高亮地址）
    std::vector<unsigned char> disassemblyBuffer;  // 反汇编数据缓冲区
    bool disassemblyBufferValid = false;  // 缓冲区数据是否有效（用于区分从未读取和读取失败）
    DisassemblyResult cachedDisassemblyResult;  // 缓存的反汇编结果
    bool disassemblyFailed = false;  // 反汇编是否已失败（避免无限重试）
    uint64_t disassemblyFailedAddress = 0;  // 反汇编失败时的地址（用于检测地址是否改变）
    bool disassemblyAutoRefresh = false;  // 是否自动刷新（简化后不再使用自动刷新）
    float disassemblyRefreshInterval = 2.0f;  // 刷新间隔（保留字段以兼容UI）
    float timeSinceDisassemblyRefresh = 0.0f;  // 距离上次刷新的时间（保留字段以兼容UI）
    bool scrollToDisassemblyAddress = false;  // 是否需要滚动到高亮地址
    static constexpr size_t DISASSEMBLY_BUFFER_SIZE = 4096;  // 反汇编缓冲区大小（4KB）
    
};
