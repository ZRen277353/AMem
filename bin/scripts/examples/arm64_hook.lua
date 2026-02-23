-- ============================================================
-- ARM64 Inline Hook 模板
-- 用于在远程 Android 进程中注入自定义代码
-- 架构: AArch64 (ARM64) only
-- ============================================================

-- ==================== ARM64 指令编码器 ====================
local arm64 = {}

-- 小端序: 将 32 位指令编码为 4 字节 table
function arm64.encode32(inst)
    return {
        bit.band(inst, 0xFF),
        bit.band(bit.rshift(inst, 8), 0xFF),
        bit.band(bit.rshift(inst, 16), 0xFF),
        bit.band(bit.rshift(inst, 24), 0xFF),
    }
end

-- 将 64 位地址编码为 8 字节 table (小端序)
function arm64.encode64(addr)
    local lo = addr % 0x100000000
    local hi = (addr - lo) / 0x100000000
    local t = {}
    for i = 0, 3 do
        t[#t + 1] = bit.band(bit.rshift(lo, i * 8), 0xFF)
    end
    for i = 0, 3 do
        t[#t + 1] = bit.band(bit.rshift(hi, i * 8), 0xFF)
    end
    return t
end

-- NOP: 0xD503201F
function arm64.NOP()
    return arm64.encode32(0xD503201F)
end

-- RET (X30): 0xD65F03C0
function arm64.RET()
    return arm64.encode32(0xD65F03C0)
end

-- B imm: 无条件跳转, 偏移量为指令数 (±128MB)
-- offset_bytes: 从当前指令到目标的字节偏移
function arm64.B(offset_bytes)
    local imm26 = offset_bytes / 4
    -- 处理负偏移 (符号扩展到 26 位)
    if imm26 < 0 then
        imm26 = imm26 + 0x4000000  -- 2^26
    end
    return arm64.encode32(bit.bor(0x14000000, bit.band(imm26, 0x3FFFFFF)))
end

-- BL imm: 带链接跳转 (±128MB)
function arm64.BL(offset_bytes)
    local imm26 = offset_bytes / 4
    if imm26 < 0 then
        imm26 = imm26 + 0x4000000
    end
    return arm64.encode32(bit.bor(0x94000000, bit.band(imm26, 0x3FFFFFF)))
end

-- BR Xn: 跳转到寄存器
function arm64.BR(rn)
    return arm64.encode32(bit.bor(0xD61F0000, bit.lshift(rn, 5)))
end

-- BLR Xn: 带链接跳转到寄存器
function arm64.BLR(rn)
    return arm64.encode32(bit.bor(0xD63F0000, bit.lshift(rn, 5)))
end

-- LDR Xn, [PC, #imm]: 从 PC 相对偏移加载 64 位值
-- imm_bytes: 字节偏移 (必须 4 字节对齐)
function arm64.LDR_literal(rt, imm_bytes)
    local imm19 = imm_bytes / 4
    if imm19 < 0 then
        imm19 = imm19 + 0x80000  -- 2^19
    end
    return arm64.encode32(bit.bor(0x58000000, bit.lshift(bit.band(imm19, 0x7FFFF), 5), rt))
end

-- STP Xt1, Xt2, [SP, #-offset]! (pre-index, 压栈)
function arm64.STP_pre(rt1, rt2, offset)
    local imm7 = offset / 8
    if imm7 < 0 then
        imm7 = imm7 + 0x80  -- 2^7
    end
    return arm64.encode32(bit.bor(
        0xA9800000,
        bit.lshift(bit.band(imm7, 0x7F), 15),
        bit.lshift(rt2, 10),
        bit.lshift(31, 5),  -- SP = X31
        rt1
    ))
end

-- LDP Xt1, Xt2, [SP], #offset (post-index, 出栈)
function arm64.LDP_post(rt1, rt2, offset)
    local imm7 = offset / 8
    if imm7 < 0 then
        imm7 = imm7 + 0x80
    end
    return arm64.encode32(bit.bor(
        0xA8C00000,
        bit.lshift(bit.band(imm7, 0x7F), 15),
        bit.lshift(rt2, 10),
        bit.lshift(31, 5),  -- SP
        rt1
    ))
end

-- MOVZ Xd, #imm16, LSL #shift (shift: 0,16,32,48)
function arm64.MOVZ(rd, imm16, shift)
    shift = shift or 0
    local hw = shift / 16
    return arm64.encode32(bit.bor(
        0xD2800000,
        bit.lshift(hw, 21),
        bit.lshift(bit.band(imm16, 0xFFFF), 5),
        rd
    ))
end

-- MOVK Xd, #imm16, LSL #shift
function arm64.MOVK(rd, imm16, shift)
    shift = shift or 0
    local hw = shift / 16
    return arm64.encode32(bit.bor(
        0xF2800000,
        bit.lshift(hw, 21),
        bit.lshift(bit.band(imm16, 0xFFFF), 5),
        rd
    ))
end

-- MOV Xd, #imm64: 用 MOVZ + MOVK 序列加载任意 64 位立即数
-- 返回 bytes table (4~16 字节)
function arm64.MOV_imm64(rd, imm64)
    local bytes = {}
    local lo = imm64 % 0x100000000
    local hi = (imm64 - lo) / 0x100000000

    local parts = {
        bit.band(lo, 0xFFFF),
        bit.band(bit.rshift(lo, 16), 0xFFFF),
        bit.band(hi, 0xFFFF),
        bit.band(bit.rshift(hi, 16), 0xFFFF),
    }

    -- 找到第一个非零 part 用 MOVZ, 其余用 MOVK
    local first = true
    for i, v in ipairs(parts) do
        if v ~= 0 or (i == 1 and first) then
            local inst
            if first then
                inst = arm64.MOVZ(rd, v, (i - 1) * 16)
                first = false
            else
                inst = arm64.MOVK(rd, v, (i - 1) * 16)
            end
            for _, b in ipairs(inst) do bytes[#bytes + 1] = b end
        end
    end
    -- 如果地址为 0, 至少生成一条 MOVZ
    if first then
        for _, b in ipairs(arm64.MOVZ(rd, 0, 0)) do bytes[#bytes + 1] = b end
    end
    return bytes
end

-- ==================== 辅助工具 ====================

-- 合并多个 bytes table
local function concat_bytes(...)
    local result = {}
    for _, t in ipairs({...}) do
        for _, b in ipairs(t) do result[#result + 1] = b end
    end
    return result
end

-- 生成远跳转 stub: LDR X16, #8; BR X16; .quad target
-- 共 12 字节, 可跳转到任意 64 位地址
function arm64.long_branch(target_addr)
    return concat_bytes(
        arm64.LDR_literal(16, 8),  -- LDR X16, [PC, #8]
        arm64.BR(16),              -- BR X16
        arm64.encode64(target_addr)
    )
end

-- 生成远调用 stub: LDR X16, #8; BLR X16; .quad target
function arm64.long_call(target_addr)
    return concat_bytes(
        arm64.LDR_literal(16, 8),  -- LDR X16, [PC, #8]
        arm64.BLR(16),             -- BLR X16
        arm64.encode64(target_addr)
    )
end

-- ==================== Hook 框架 ====================
local Hook = {}
Hook.__index = Hook

--[[
    创建一个新的 Hook 实例

    参数:
        target_addr   : 要 hook 的目标地址 (函数入口或指令地址)
        cave_addr     : code cave 地址 (可写可执行的空闲内存区域)
        hook_bytes    : 自定义 hook 代码的字节 table (ARM64 机器码)
                        在这段代码中可以自由使用 X0-X15 (需自行保存恢复)
        options       : 可选配置 table
            .inst_count   : 要替换的原始指令数量 (默认 1, 最少覆盖跳转所需字节)
            .save_regs    : 是否在 hook 代码前后保存/恢复 X0-X15 (默认 true)
            .use_short_branch : 强制使用 4 字节 B 跳转 (目标必须在 ±128MB 内)
]]
function Hook.new(target_addr, cave_addr, hook_bytes, options)
    local self = setmetatable({}, Hook)
    options = options or {}

    self.target_addr = target_addr
    self.cave_addr = cave_addr
    self.hook_bytes = hook_bytes or {}
    self.inst_count = options.inst_count or 3  -- 默认替换 3 条指令 (12字节, 够放 long_branch)
    self.save_regs = options.save_regs ~= false
    self.use_short = options.use_short_branch or false
    self.original_bytes = nil  -- 备份的原始字节
    self.installed = false

    return self
end

-- 构建 code cave 中的 trampoline 代码
function Hook:_build_trampoline()
    local code = {}
    local append = function(bytes)
        for _, b in ipairs(bytes) do code[#code + 1] = b end
    end

    -- 1) 保存寄存器 (如果启用)
    if self.save_regs then
        append(arm64.STP_pre(29, 30, -16))   -- STP X29, X30, [SP, #-16]!
        append(arm64.STP_pre(0,  1,  -16))   -- STP X0,  X1,  [SP, #-16]!
        append(arm64.STP_pre(2,  3,  -16))   -- STP X2,  X3,  [SP, #-16]!
        append(arm64.STP_pre(4,  5,  -16))   -- STP X4,  X5,  [SP, #-16]!
        append(arm64.STP_pre(6,  7,  -16))   -- STP X6,  X7,  [SP, #-16]!
    end

    -- 2) 用户自定义 hook 代码
    append(self.hook_bytes)

    -- 3) 恢复寄存器
    if self.save_regs then
        append(arm64.LDP_post(6,  7,  16))   -- LDP X6,  X7,  [SP], #16
        append(arm64.LDP_post(4,  5,  16))   -- LDP X4,  X5,  [SP], #16
        append(arm64.LDP_post(2,  3,  16))   -- LDP X2,  X3,  [SP], #16
        append(arm64.LDP_post(0,  1,  16))   -- LDP X0,  X1,  [SP], #16
        append(arm64.LDP_post(29, 30, 16))   -- LDP X29, X30, [SP], #16
    end

    -- 4) 执行被覆盖的原始指令
    if self.original_bytes then
        append(self.original_bytes)
    end

    -- 5) 跳回原始代码 (hook 点之后)
    local return_addr = self.target_addr + self.inst_count * 4
    append(arm64.long_branch(return_addr))

    return code
end

-- 安装 hook
function Hook:install()
    if self.installed then
        log("[Hook] 已经安装, 跳过")
        return false
    end

    local patch_size = self.inst_count * 4

    -- 1) 备份原始指令
    local orig = mem.read(self.target_addr, patch_size)
    if not orig then
        log("[Hook] 读取原始指令失败: " .. string.format("0x%X", self.target_addr))
        return false
    end
    self.original_bytes = orig
    log(string.format("[Hook] 备份 %d 字节原始指令 @ 0x%X", patch_size, self.target_addr))

    -- 2) 构建并写入 trampoline
    local trampoline = self:_build_trampoline()
    local ok = mem.write(self.cave_addr, trampoline)
    if not ok then
        log("[Hook] 写入 trampoline 失败: " .. string.format("0x%X", self.cave_addr))
        return false
    end
    log(string.format("[Hook] Trampoline (%d 字节) 写入 @ 0x%X", #trampoline, self.cave_addr))

    -- 3) 在目标地址写入跳转到 code cave
    local jump
    if self.use_short then
        local offset = self.cave_addr - self.target_addr
        jump = arm64.B(offset)
        -- 用 NOP 填充剩余空间
        for i = 2, self.inst_count do
            jump = concat_bytes(jump, arm64.NOP())
        end
    else
        jump = arm64.long_branch(self.cave_addr)
        -- long_branch 占 12 字节 (3 条指令), 用 NOP 填充剩余
        for i = 4, self.inst_count do
            jump = concat_bytes(jump, arm64.NOP())
        end
    end

    ok = mem.write(self.target_addr, jump)
    if not ok then
        log("[Hook] 写入跳转指令失败")
        return false
    end

    self.installed = true
    log(string.format("[Hook] 安装成功! 0x%X -> 0x%X", self.target_addr, self.cave_addr))
    return true
end

-- 卸载 hook, 恢复原始指令
function Hook:uninstall()
    if not self.installed then
        log("[Hook] 未安装, 跳过")
        return false
    end
    if not self.original_bytes then
        log("[Hook] 无原始字节备份, 无法恢复")
        return false
    end

    local ok = mem.write(self.target_addr, self.original_bytes)
    if not ok then
        log("[Hook] 恢复原始指令失败")
        return false
    end

    self.installed = false
    log(string.format("[Hook] 已卸载, 原始指令已恢复 @ 0x%X", self.target_addr))
    return true
end

-- ==================== Code Cave 搜索工具 ====================

-- 在指定模块中搜索连续的零字节区域作为 code cave
-- module_name: 模块名
-- min_size: 最小空间 (字节)
-- search_from_end: 从模块末尾开始搜索 (默认 true, 末尾更容易找到填充区)
function Hook.find_cave(module_name, min_size, search_from_end)
    min_size = min_size or 128
    search_from_end = search_from_end ~= false

    local modules = module.list()
    if not modules then
        log("[Cave] 获取模块列表失败")
        return nil
    end

    local target_mod = nil
    for _, m in ipairs(modules) do
        if m.name:find(module_name, 1, true) then
            target_mod = m
            break
        end
    end

    if not target_mod then
        log("[Cave] 未找到模块: " .. module_name)
        return nil
    end

    local base = target_mod.base
    local size = target_mod.size
    log(string.format("[Cave] 搜索模块 %s: base=0x%X size=0x%X", target_mod.name, base, size))

    -- 从模块末尾向前搜索 (通常 .text 段末尾有对齐填充)
    local block_size = 4096
    local scan_start = search_from_end and (base + size - block_size) or base
    local scan_end = search_from_end and base or (base + size)
    local step = search_from_end and -block_size or block_size

    local addr = scan_start
    while (search_from_end and addr >= scan_end) or (not search_from_end and addr < scan_end) do
        local data = mem.read(addr, block_size)
        if data then
            local zero_start = nil
            local zero_count = 0
            for i, b in ipairs(data) do
                if b == 0 then
                    if not zero_start then zero_start = i end
                    zero_count = zero_count + 1
                    if zero_count >= min_size then
                        local cave = addr + zero_start - 1
                        -- 对齐到 4 字节
                        cave = cave + (4 - cave % 4) % 4
                        log(string.format("[Cave] 找到 code cave @ 0x%X (%d 字节可用)", cave, zero_count))
                        return cave
                    end
                else
                    zero_start = nil
                    zero_count = 0
                end
            end
        end
        addr = addr + step
    end

    log("[Cave] 未找到足够大的 code cave")
    return nil
end

-- ==================== 使用示例 ====================
--[[

-- 示例 1: 基本 inline hook — 将某函数的返回值改为固定值
-- 假设目标函数在 libgame.so 偏移 0x12345 处, 我们要让它返回 999

local lib_base = module.getBase("libgame.so")
if not lib_base then
    log("找不到 libgame.so")
    return
end

local target = lib_base + 0x12345

-- 搜索 code cave
local cave = Hook.find_cave("libgame.so", 128)
if not cave then
    log("找不到 code cave, 请手动指定地址")
    return
end

-- 构建 hook 代码: MOV W0, #999 (将返回值设为 999)
-- W0 = X0 的低 32 位, MOVZ W0, #999 = 0x5280_7CE0
local hook_code = arm64.encode32(0x52807CE0)  -- MOVZ W0, #999

local hook = Hook.new(target, cave, hook_code, {
    inst_count = 3,     -- 替换 3 条指令 (12 字节, 用于 long_branch)
    save_regs = false,  -- 不保存寄存器 (我们就是要改 X0)
})

hook:install()

-- 需要恢复时:
-- hook:uninstall()


-- 示例 2: 带寄存器保存的 hook — 记录函数调用参数
-- 将 X0 (第一个参数) 写入一个已知地址供外部读取

local lib_base = module.getBase("libgame.so")
local target = lib_base + 0x67890
local cave = Hook.find_cave("libgame.so", 256)

-- 构建 hook 代码: 将 X0 存储到 log_addr
local log_addr = cave + 200  -- 在 cave 末尾留一块空间存数据

local hook_code = concat_bytes(
    -- 加载 log_addr 到 X17
    arm64.MOV_imm64(17, log_addr),
    -- STR X0, [X17]: 0xF9000220
    arm64.encode32(0xF9000220)
)

local hook = Hook.new(target, cave, hook_code, {
    inst_count = 3,
    save_regs = true,  -- 保存并恢复所有通用寄存器
})

hook:install()

-- 读取记录的参数值:
-- local arg0 = mem.readLong(log_addr)
-- log("X0 = " .. string.format("0x%X", arg0))


-- 示例 3: 函数替换 — 直接跳过原函数, 返回自定义值

local lib_base = module.getBase("libgame.so")
local target = lib_base + 0xABCDE
local cave = lib_base + 0xF0000  -- 手动指定 cave 地址

-- hook 代码: MOV X0, #1; RET (直接返回 1, 跳过原函数)
local hook_code = concat_bytes(
    arm64.MOVZ(0, 1, 0),  -- MOV X0, #1
    arm64.RET()            -- RET
)

-- inst_count=0 表示不执行原始指令 (函数替换模式)
-- 但仍需覆盖至少 3 条指令来放置 long_branch
local ok = mem.write(target, arm64.long_branch(cave))
if ok then
    mem.write(cave, hook_code)
    log("函数替换完成")
end

]]

log("ARM64 Hook 模板已加载")
log("用法: local hook = Hook.new(target, cave, code, opts)")
log("      hook:install()  / hook:uninstall()")

return {
    arm64 = arm64,
    Hook = Hook,
    concat_bytes = concat_bytes,
}
