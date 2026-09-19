#include "maro_Trace.hpp"

#include <dbghelp.h>
#include <cvconst.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace
{
std::mutex maro_symbolsMutex;

bool maro_CancelRequested(const std::function<bool()>& maro_cancelled) noexcept
{
    try { return maro_cancelled && maro_cancelled(); }
    catch (...) { return true; }
}

std::wstring maro_TracePath(std::wstring maro_path)
{
    std::replace(maro_path.begin(), maro_path.end(), L'/', L'\\');
    std::transform(maro_path.begin(), maro_path.end(), maro_path.begin(),
        [](wchar_t maro_char) { return static_cast<wchar_t>(towlower(maro_char)); });
    return maro_path;
}

std::wstring maro_Hex(std::uint64_t maro_value)
{
    std::wostringstream maro_text;
    maro_text << L"0x" << std::hex << maro_value;
    return maro_text.str();
}

struct maro_Breakpoint
{
    DWORD64 maro_address = 0;
    DWORD maro_line = 0;
    BYTE maro_original = 0;
    bool maro_armed = false;
};

struct maro_LineCollector
{
    std::wstring maro_source;
    std::map<DWORD64, maro_Breakpoint>* maro_breakpoints = nullptr;
    bool maro_failed = false;
};

BOOL CALLBACK maro_CollectLine(PSRCCODEINFOW maro_line, PVOID maro_context)
{
    auto& maro_collector = *static_cast<maro_LineCollector*>(maro_context);
    try
    {
        if (maro_TracePath(maro_line->FileName) == maro_collector.maro_source && maro_line->LineNumber)
        {
            if (maro_collector.maro_breakpoints->size() >= 32'768)
            {
                maro_collector.maro_failed = true;
                return FALSE;
            }
            maro_collector.maro_breakpoints->try_emplace(maro_line->Address,
                maro_Breakpoint{maro_line->Address, maro_line->LineNumber});
        }
        return TRUE;
    }
    catch (...) { maro_collector.maro_failed = true; return FALSE; }
}

bool maro_WriteInstruction(HANDLE maro_process, DWORD64 maro_address, BYTE maro_byte)
{
    SIZE_T maro_written = 0;
    const auto maro_pointer = reinterpret_cast<void*>(maro_address);
    return WriteProcessMemory(maro_process, maro_pointer, &maro_byte, 1, &maro_written) &&
        maro_written == 1 && FlushInstructionCache(maro_process, maro_pointer, 1);
}

bool maro_Arm(HANDLE maro_process, maro_Breakpoint& maro_breakpoint)
{
    if (maro_breakpoint.maro_armed) return true;
    if (!maro_WriteInstruction(maro_process, maro_breakpoint.maro_address, 0xcc)) return false;
    maro_breakpoint.maro_armed = true;
    return true;
}

bool maro_Disarm(HANDLE maro_process, maro_Breakpoint& maro_breakpoint)
{
    if (!maro_breakpoint.maro_armed) return true;
    if (!maro_WriteInstruction(maro_process, maro_breakpoint.maro_address, maro_breakpoint.maro_original))
        return false;
    maro_breakpoint.maro_armed = false;
    return true;
}

bool maro_RegisterValue(const CONTEXT& maro_context, ULONG maro_register, DWORD64& maro_value)
{
    switch (maro_register)
    {
    case CV_AMD64_RAX: case CV_AMD64_EAX: maro_value = maro_context.Rax; return true;
    case CV_AMD64_RBX: case CV_AMD64_EBX: maro_value = maro_context.Rbx; return true;
    case CV_AMD64_RCX: case CV_AMD64_ECX: maro_value = maro_context.Rcx; return true;
    case CV_AMD64_RDX: case CV_AMD64_EDX: maro_value = maro_context.Rdx; return true;
    case CV_AMD64_RSI: case CV_AMD64_ESI: maro_value = maro_context.Rsi; return true;
    case CV_AMD64_RDI: case CV_AMD64_EDI: maro_value = maro_context.Rdi; return true;
    case CV_AMD64_RBP: case CV_AMD64_EBP: maro_value = maro_context.Rbp; return true;
    case CV_AMD64_RSP: case CV_AMD64_ESP: maro_value = maro_context.Rsp; return true;
    case CV_AMD64_R8: case CV_AMD64_R8D: maro_value = maro_context.R8; return true;
    case CV_AMD64_R9: case CV_AMD64_R9D: maro_value = maro_context.R9; return true;
    case CV_AMD64_R10: case CV_AMD64_R10D: maro_value = maro_context.R10; return true;
    case CV_AMD64_R11: case CV_AMD64_R11D: maro_value = maro_context.R11; return true;
    case CV_AMD64_R12: case CV_AMD64_R12D: maro_value = maro_context.R12; return true;
    case CV_AMD64_R13: case CV_AMD64_R13D: maro_value = maro_context.R13; return true;
    case CV_AMD64_R14: case CV_AMD64_R14D: maro_value = maro_context.R14; return true;
    case CV_AMD64_R15: case CV_AMD64_R15D: maro_value = maro_context.R15; return true;
    default: return false;
    }
}

struct maro_VariableCollector
{
    HANDLE maro_process = nullptr;
    const CONTEXT* maro_context = nullptr;
    std::vector<maro_TraceVariable>* maro_variables = nullptr;
    bool maro_frameReady = false;
    std::size_t maro_remainingCells = 512;
};

struct maro_TypeInfo
{
    ULONG maro_id = 0;
    DWORD maro_tag = 0;
    DWORD maro_basic = 0;
    ULONG64 maro_size = 0;
    std::wstring maro_name = L"?";
};

maro_TypeInfo maro_ReadType(HANDLE maro_process, DWORD64 maro_module, ULONG maro_id)
{
    maro_TypeInfo maro_info;
    maro_info.maro_id = maro_id;
    for (unsigned maro_depth = 0; maro_depth < 8; ++maro_depth)
    {
        if (!SymGetTypeInfo(maro_process, maro_module, maro_info.maro_id, TI_GET_SYMTAG, &maro_info.maro_tag))
            return maro_info;
        if (maro_info.maro_tag != SymTagTypedef) break;
        ULONG maro_next = 0;
        if (!SymGetTypeInfo(maro_process, maro_module, maro_info.maro_id, TI_GET_TYPEID, &maro_next))
            return maro_info;
        maro_info.maro_id = maro_next;
    }
    SymGetTypeInfo(maro_process, maro_module, maro_info.maro_id, TI_GET_LENGTH, &maro_info.maro_size);
    if (maro_info.maro_tag == SymTagPointerType) maro_info.maro_name = L"pointer";
    else if (maro_info.maro_tag == SymTagArrayType) maro_info.maro_name = L"array";
    else if (maro_info.maro_tag == SymTagUDT) maro_info.maro_name = L"struct / class";
    else if (maro_info.maro_tag == SymTagEnum) maro_info.maro_name = L"enum";
    else if (maro_info.maro_tag == SymTagBaseType && SymGetTypeInfo(maro_process,
        maro_module, maro_info.maro_id, TI_GET_BASETYPE, &maro_info.maro_basic))
    {
        switch (maro_info.maro_basic)
        {
        case btChar: maro_info.maro_name = L"char"; break;
        case btWChar: maro_info.maro_name = L"wchar_t"; break;
        case btBool: maro_info.maro_name = L"bool"; break;
        case btInt: maro_info.maro_name = maro_info.maro_size == 8 ? L"int64" : maro_info.maro_size == 2 ? L"short" : L"int"; break;
        case btLong: maro_info.maro_name = L"long"; break;
        case btUInt: maro_info.maro_name = maro_info.maro_size == 8 ? L"uint64" : L"unsigned int"; break;
        case btULong: maro_info.maro_name = L"unsigned long"; break;
        case btFloat: maro_info.maro_name = maro_info.maro_size == 4 ? L"float" : L"double"; break;
        default: break;
        }
    }
    return maro_info;
}

bool maro_IsScalar(const maro_TypeInfo& maro_type)
{
    return maro_type.maro_size > 0 && maro_type.maro_size <= sizeof(DWORD64) &&
        (maro_type.maro_tag == SymTagPointerType || (maro_type.maro_tag == SymTagBaseType &&
        (maro_type.maro_basic == btInt || maro_type.maro_basic == btUInt || maro_type.maro_basic == btLong ||
            maro_type.maro_basic == btULong || maro_type.maro_basic == btChar || maro_type.maro_basic == btWChar ||
            maro_type.maro_basic == btBool || maro_type.maro_basic == btFloat)));
}

void maro_FormatScalar(maro_TraceVariable& maro_variable, const maro_TypeInfo& maro_type, DWORD64 maro_raw)
{
    const auto maro_bits = static_cast<unsigned>(maro_type.maro_size * 8);
    const DWORD64 maro_mask = maro_bits == 64 ? ~DWORD64{0} : (DWORD64{1} << maro_bits) - 1;
    maro_raw &= maro_mask;
    if (maro_raw == (0xccccccccccccccccull & maro_mask) || maro_raw == (0xcdcdcdcdcdcdcdcdull & maro_mask))
    {
        maro_variable.maro_value = L"초기화 미확인";
        return;
    }
    if (maro_type.maro_tag == SymTagPointerType) maro_variable.maro_value = maro_Hex(maro_raw);
    else if (maro_type.maro_basic == btFloat && maro_type.maro_size == 4)
    {
        float maro_number = 0;
        std::memcpy(&maro_number, &maro_raw, sizeof(maro_number));
        maro_variable.maro_value = std::to_wstring(maro_number);
    }
    else if (maro_type.maro_basic == btFloat && maro_type.maro_size == 8)
    {
        double maro_number = 0;
        std::memcpy(&maro_number, &maro_raw, sizeof(maro_number));
        maro_variable.maro_value = std::to_wstring(maro_number);
    }
    else if (maro_type.maro_basic == btInt || maro_type.maro_basic == btLong || maro_type.maro_basic == btChar)
    {
        const auto maro_signed = static_cast<std::int64_t>(maro_raw << (64 - maro_bits)) >> (64 - maro_bits);
        maro_variable.maro_value = std::to_wstring(maro_signed);
    }
    else if (maro_type.maro_basic == btBool && maro_raw <= 1)
        maro_variable.maro_value = maro_raw ? L"true" : L"false";
    else maro_variable.maro_value = std::to_wstring(maro_raw);
    maro_variable.maro_available = true;
}

void maro_ReadScalar(HANDLE maro_process, maro_TraceVariable& maro_variable, const maro_TypeInfo& maro_type)
{
    DWORD64 maro_raw = 0;
    SIZE_T maro_readSize = 0;
    if (maro_IsScalar(maro_type) && maro_variable.maro_address &&
        maro_variable.maro_address <= (std::numeric_limits<DWORD64>::max)() - maro_type.maro_size &&
        ReadProcessMemory(maro_process, reinterpret_cast<const void*>(maro_variable.maro_address),
            &maro_raw, static_cast<SIZE_T>(maro_type.maro_size), &maro_readSize) && maro_readSize == maro_type.maro_size)
        maro_FormatScalar(maro_variable, maro_type, maro_raw);
}

void maro_ReadArray(maro_VariableCollector& maro_collector, DWORD64 maro_module,
    maro_TraceVariable& maro_variable, maro_TypeInfo maro_type, bool maro_hasAddress)
{
    ULONG64 maro_count = 1;
    while (maro_type.maro_tag == SymTagArrayType)
    {
        DWORD maro_length = 0;
        ULONG maro_next = 0;
        if (maro_variable.maro_dimensions.size() == 3 ||
            !SymGetTypeInfo(maro_collector.maro_process, maro_module, maro_type.maro_id, TI_GET_COUNT, &maro_length) ||
            !maro_length || !SymGetTypeInfo(maro_collector.maro_process, maro_module,
                maro_type.maro_id, TI_GET_TYPEID, &maro_next))
        {
            maro_variable.maro_truncated = true;
            maro_variable.maro_value = L"배열 차원 관측 불가 · 최대 3차원";
            return;
        }
        auto maro_element = maro_ReadType(maro_collector.maro_process, maro_module, maro_next);
        if (!maro_element.maro_size || maro_type.maro_size / maro_element.maro_size != maro_length ||
            maro_type.maro_size % maro_element.maro_size ||
            maro_count > (std::numeric_limits<ULONG64>::max)() / maro_length)
        {
            maro_variable.maro_value = L"배열 배치 관측 불가";
            return;
        }
        maro_count *= maro_length;
        maro_variable.maro_dimensions.push_back(maro_length);
        maro_type = std::move(maro_element);
    }
    maro_variable.maro_type = maro_type.maro_name;
    for (const auto maro_dimension : maro_variable.maro_dimensions)
        maro_variable.maro_type += L"[" + std::to_wstring(maro_dimension) + L"]";
    if (!maro_IsScalar(maro_type))
    {
        maro_variable.maro_value = L"복합 형식 배열 관측 미지원";
        return;
    }
    if (!maro_hasAddress) return;
    const auto maro_shown = static_cast<std::size_t>((std::min)({maro_count, ULONG64{64},
        static_cast<ULONG64>(maro_collector.maro_remainingCells)}));
    maro_variable.maro_truncated = maro_shown < maro_count;
    maro_variable.maro_children.reserve(maro_shown);
    for (std::size_t maro_index = 0; maro_index < maro_shown; ++maro_index)
    {
        maro_TraceVariable maro_cell;
        std::size_t maro_remainder = maro_index;
        for (auto maro_dimension = maro_variable.maro_dimensions.rbegin();
            maro_dimension != maro_variable.maro_dimensions.rend(); ++maro_dimension)
        {
            maro_cell.maro_name = L"[" + std::to_wstring(maro_remainder % *maro_dimension) + L"]" + maro_cell.maro_name;
            maro_remainder /= *maro_dimension;
        }
        maro_cell.maro_type = maro_type.maro_name;
        maro_cell.maro_value = L"관측 불가";
        const ULONG64 maro_offset = maro_index * maro_type.maro_size;
        if (maro_variable.maro_address <= (std::numeric_limits<DWORD64>::max)() - maro_offset)
        {
            maro_cell.maro_address = maro_variable.maro_address + maro_offset;
            maro_ReadScalar(maro_collector.maro_process, maro_cell, maro_type);
        }
        maro_variable.maro_available = maro_variable.maro_available || maro_cell.maro_available;
        maro_variable.maro_children.push_back(std::move(maro_cell));
    }
    maro_collector.maro_remainingCells -= maro_shown;
    maro_variable.maro_value = std::to_wstring(maro_shown) + L" / " + std::to_wstring(maro_count) + L"개 원소";
    if (maro_variable.maro_truncated) maro_variable.maro_value += L" · 표시 제한";
}

BOOL CALLBACK maro_CollectVariable(PSYMBOL_INFOW maro_symbol, ULONG, PVOID maro_context)
{
    auto& maro_collector = *static_cast<maro_VariableCollector*>(maro_context);
    if (maro_collector.maro_variables->size() >= 128) return FALSE;
    if (maro_symbol->Tag != SymTagData ||
        !(maro_symbol->Flags & (SYMFLAG_LOCAL | SYMFLAG_PARAMETER))) return TRUE;
    try
    {
        maro_TraceVariable maro_variable;
        maro_variable.maro_name.assign(maro_symbol->Name,
            wcsnlen_s(maro_symbol->Name, maro_symbol->NameLen));
        maro_variable.maro_value = L"관측 불가";
        const auto maro_type = maro_ReadType(maro_collector.maro_process, maro_symbol->ModBase, maro_symbol->TypeIndex);
        maro_variable.maro_type = maro_type.maro_name;
        DWORD64 maro_raw = 0;
        bool maro_read = false;
        bool maro_hasAddress = false;
        if (maro_collector.maro_frameReady &&
            !(maro_symbol->Flags & SYMFLAG_TLSREL))
        {
            if (maro_symbol->Flags & (SYMFLAG_VALUEPRESENT | SYMFLAG_REGISTER))
            {
                if (maro_IsScalar(maro_type))
                {
                    if (maro_symbol->Flags & SYMFLAG_VALUEPRESENT)
                    {
                        maro_raw = maro_symbol->Value;
                        maro_read = true;
                    }
                    else maro_read = maro_RegisterValue(*maro_collector.maro_context, maro_symbol->Register, maro_raw);
                }
            }
            else
            {
                DWORD64 maro_base = 0;
                maro_hasAddress = true;
                if (maro_symbol->Flags & SYMFLAG_REGREL)
                    maro_hasAddress = maro_RegisterValue(*maro_collector.maro_context, maro_symbol->Register, maro_base);
                else if (maro_symbol->Flags & SYMFLAG_FRAMEREL)
                    maro_hasAddress = false;
                maro_variable.maro_address = maro_base + maro_symbol->Address;
                if (maro_hasAddress && maro_IsScalar(maro_type))
                    maro_ReadScalar(maro_collector.maro_process, maro_variable, maro_type);
            }
        }
        if (maro_read) maro_FormatScalar(maro_variable, maro_type, maro_raw);
        else if (maro_type.maro_tag == SymTagArrayType)
            maro_ReadArray(maro_collector, maro_symbol->ModBase, maro_variable, maro_type, maro_hasAddress);
        else if (maro_type.maro_tag == SymTagUDT)
            maro_variable.maro_value = L"복합 형식 관측 미지원";
        maro_collector.maro_variables->push_back(std::move(maro_variable));
        return TRUE;
    }
    catch (...) { return FALSE; }
}

maro_TraceFrame maro_ReadFrame(HANDLE maro_process, DWORD64 maro_address, bool* maro_atEntry = nullptr)
{
    maro_TraceFrame maro_frame;
    alignas(SYMBOL_INFOW) std::array<std::byte,
        sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)> maro_storage{};
    auto maro_symbol = reinterpret_cast<SYMBOL_INFOW*>(maro_storage.data());
    maro_symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    maro_symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 maro_displacement = 0;
    if (SymFromAddrW(maro_process, maro_address, &maro_displacement, maro_symbol))
    {
        maro_frame.maro_function.assign(maro_symbol->Name,
            wcsnlen_s(maro_symbol->Name, maro_symbol->NameLen));
        if (maro_atEntry) *maro_atEntry = maro_displacement == 0;
    }
    else maro_frame.maro_function = maro_Hex(maro_address);
    IMAGEHLP_LINEW64 maro_line{};
    maro_line.SizeOfStruct = sizeof(maro_line);
    DWORD maro_lineDisplacement = 0;
    if (SymGetLineFromAddrW64(maro_process, maro_address, &maro_lineDisplacement, &maro_line))
    {
        if (maro_line.FileName) maro_frame.maro_file = maro_line.FileName;
        maro_frame.maro_line = maro_line.LineNumber;
    }
    return maro_frame;
}

maro_TraceSnapshot maro_Capture(HANDLE maro_process, HANDLE maro_thread,
    CONTEXT maro_context, const std::wstring& maro_source, DWORD maro_line)
{
    std::lock_guard maro_lock(maro_symbolsMutex);
    maro_TraceSnapshot maro_snapshot;
    maro_snapshot.maro_file = maro_source;
    maro_snapshot.maro_line = maro_line;
    maro_snapshot.maro_waiting = true;
    maro_snapshot.maro_message = L"표시 행 실행 직전 · 원시 관측값 · 초기화 여부 미확인";
    bool maro_atEntry = true;
    maro_snapshot.maro_frames.push_back(maro_ReadFrame(maro_process, maro_context.Rip, &maro_atEntry));
    maro_snapshot.maro_function = maro_snapshot.maro_frames.front().maro_function;
    IMAGEHLP_STACK_FRAME maro_scope{};
    maro_scope.InstructionOffset = maro_context.Rip;
    maro_scope.StackOffset = maro_context.Rsp;
    maro_scope.FrameOffset = maro_context.Rbp;
    SetLastError(ERROR_SUCCESS);
    if (SymSetContext(maro_process, &maro_scope, nullptr) || GetLastError() == ERROR_SUCCESS)
    {
        maro_VariableCollector maro_collector{maro_process, &maro_context,
            &maro_snapshot.maro_variables, !maro_atEntry};
        SymEnumSymbolsW(maro_process, 0, nullptr, maro_CollectVariable, &maro_collector);
    }
    STACKFRAME64 maro_stack{};
    maro_stack.AddrPC.Offset = maro_context.Rip;
    maro_stack.AddrPC.Mode = AddrModeFlat;
    maro_stack.AddrStack.Offset = maro_context.Rsp;
    maro_stack.AddrStack.Mode = AddrModeFlat;
    maro_stack.AddrFrame.Offset = maro_context.Rbp;
    maro_stack.AddrFrame.Mode = AddrModeFlat;
    DWORD64 maro_previousPc = maro_context.Rip;
    DWORD64 maro_previousStack = maro_context.Rsp;
    for (unsigned maro_index = 0; maro_index < 64; ++maro_index)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, maro_process, maro_thread, &maro_stack,
                &maro_context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !maro_stack.AddrPC.Offset) break;
        if (maro_stack.AddrPC.Offset == maro_previousPc && maro_stack.AddrStack.Offset == maro_previousStack)
        {
            if (maro_index == 0) continue;
            break;
        }
        maro_previousPc = maro_stack.AddrPC.Offset;
        maro_previousStack = maro_stack.AddrStack.Offset;
        maro_snapshot.maro_frames.push_back(maro_ReadFrame(maro_process, maro_stack.AddrPC.Offset));
        if (maro_snapshot.maro_frames.size() >= 64) break;
    }
    return maro_snapshot;
}
}

maro_TraceSession::maro_TraceSession(maro_Callback maro_callback)
    : maro_callback_(std::move(maro_callback)) {}

void maro_TraceSession::maro_Step() noexcept
{
    if (maro_paused_.load())
    {
        maro_action_.store(maro_TraceAction::maro_step);
        maro_changed_.notify_all();
    }
}

void maro_TraceSession::maro_Continue() noexcept
{
    maro_continuing_.store(true);
    maro_action_.store(maro_TraceAction::maro_continue);
    maro_changed_.notify_all();
}

void maro_TraceSession::maro_Cancel() noexcept
{
    maro_cancelled_.store(true);
    maro_action_.store(maro_TraceAction::maro_cancel);
    maro_changed_.notify_all();
}

bool maro_TraceSession::maro_IsPaused() const noexcept { return maro_paused_.load(); }
bool maro_TraceSession::maro_IsCancelled() const noexcept { return maro_cancelled_.load(); }
bool maro_TraceSession::maro_IsContinuing() const noexcept { return maro_continuing_.load(); }

void maro_TraceSession::maro_Publish(const maro_TraceSnapshot& maro_snapshot) noexcept
{
    try { if (maro_callback_) maro_callback_(maro_snapshot); }
    catch (...) { maro_Cancel(); }
}

maro_TraceAction maro_TraceSession::maro_Pause(maro_TraceSnapshot maro_snapshot,
    const std::function<bool()>& maro_cancelled) noexcept
{
    maro_action_.store(maro_TraceAction::maro_wait);
    maro_paused_.store(true);
    maro_Publish(maro_snapshot);
    for (;;)
    {
        if (maro_IsCancelled() || maro_CancelRequested(maro_cancelled))
        {
            maro_paused_.store(false);
            return maro_TraceAction::maro_cancel;
        }
        const auto maro_action = maro_action_.load();
        if (maro_action != maro_TraceAction::maro_wait || maro_IsContinuing())
        {
            maro_paused_.store(false);
            return maro_IsContinuing() ? maro_TraceAction::maro_continue : maro_action;
        }
        std::unique_lock maro_lock(maro_waitMutex_);
        maro_changed_.wait_for(maro_lock, std::chrono::milliseconds(20), [&] {
            return maro_action_.load() != maro_TraceAction::maro_wait || maro_IsCancelled() || maro_IsContinuing();
        });
    }
}

void maro_TraceSession::maro_Finish() noexcept { maro_paused_.store(false); }

maro_TraceResult maro_RunDebugLoop(HANDLE maro_process, DWORD maro_pid,
    const std::wstring& maro_executable, const std::wstring& maro_source,
    const std::shared_ptr<maro_TraceSession>& maro_session,
    const std::function<bool()>& maro_cancelled)
{
    maro_TraceResult maro_result;
    std::map<DWORD64, maro_Breakpoint> maro_breakpoints;
    std::map<DWORD, HANDLE> maro_threads;
    std::vector<HANDLE> maro_suspended;
    DWORD64 maro_rearm = 0;
    DWORD maro_stepThread = 0;
    bool maro_symbolsReady = false;
    bool maro_initialBreakpoint = false;
    bool maro_stopping = false;
    ULONGLONG maro_stopStarted = 0;
    auto maro_fail = [&](DWORD maro_error, const wchar_t* maro_message) {
        if (!maro_result.maro_error) maro_result.maro_error = maro_error ? maro_error : ERROR_GEN_FAILURE;
        if (maro_session)
        {
            maro_TraceSnapshot maro_snapshot;
            maro_snapshot.maro_message = maro_message;
            maro_session->maro_Publish(maro_snapshot);
        }
        if (!maro_stopping)
        {
            maro_stopping = true;
            maro_stopStarted = GetTickCount64();
            TerminateProcess(maro_process, maro_result.maro_error);
        }
    };
    auto maro_resumeOthers = [&] {
        for (const auto maro_thread : maro_suspended) ResumeThread(maro_thread);
        maro_suspended.clear();
    };
    if (!maro_session || !maro_process || maro_source.empty())
        maro_fail(ERROR_INVALID_PARAMETER, L"시각화할 소스 경로가 없습니다.");
    for (;;)
    {
        if (!maro_stopping && (maro_CancelRequested(maro_cancelled) ||
            (maro_session && maro_session->maro_IsCancelled())))
        {
            maro_result.maro_cancelled = true;
            maro_stopping = true;
            maro_stopStarted = GetTickCount64();
            TerminateProcess(maro_process, ERROR_CANCELLED);
        }
        DEBUG_EVENT maro_event{};
        if (!WaitForDebugEvent(&maro_event, 20))
        {
            const DWORD maro_error = GetLastError();
            if (maro_error != ERROR_SEM_TIMEOUT && maro_error != ERROR_SUCCESS)
                maro_fail(maro_error, L"네이티브 디버그 이벤트를 읽을 수 없습니다.");
            if (maro_stopping && GetTickCount64() - maro_stopStarted >= 2'000)
            {
                DebugActiveProcessStop(maro_pid);
                break;
            }
            continue;
        }
        DWORD maro_continue = DBG_CONTINUE;
        bool maro_exit = false;
        try
        {
            switch (maro_event.dwDebugEventCode)
            {
            case CREATE_PROCESS_DEBUG_EVENT:
            {
                const auto& maro_info = maro_event.u.CreateProcessInfo;
                maro_threads.emplace(maro_event.dwThreadId, maro_info.hThread);
                {
                    std::lock_guard maro_lock(maro_symbolsMutex);
                    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME |
                        SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_DEFERRED_LOADS);
                    const auto maro_search = std::filesystem::path(maro_executable).parent_path().wstring();
                    maro_symbolsReady = SymInitializeW(maro_process, maro_search.c_str(), FALSE) != FALSE;
                    if (maro_symbolsReady)
                    {
                        const DWORD64 maro_base = reinterpret_cast<DWORD64>(maro_info.lpBaseOfImage);
                        if (SymLoadModuleExW(maro_process, maro_info.hFile, maro_executable.c_str(),
                                nullptr, maro_base, 0, nullptr, 0))
                        {
                            maro_LineCollector maro_collector{maro_TracePath(maro_source), &maro_breakpoints};
                            SymEnumLinesW(maro_process, maro_base, nullptr, nullptr, maro_CollectLine, &maro_collector);
                            if (maro_collector.maro_failed) maro_breakpoints.clear();
                        }
                    }
                }
                if (maro_info.hFile) CloseHandle(maro_info.hFile);
                if (maro_breakpoints.empty())
                    maro_fail(ERROR_NOT_FOUND, L"소스 행 디버그 정보가 없습니다. /Z7 /Od /DEBUG 빌드가 필요합니다.");
                else
                {
                    for (auto& [maro_address, maro_breakpoint] : maro_breakpoints)
                    {
                        SIZE_T maro_read = 0;
                        if (!ReadProcessMemory(maro_process, reinterpret_cast<const void*>(maro_address),
                                &maro_breakpoint.maro_original, 1, &maro_read) || maro_read != 1 ||
                            maro_breakpoint.maro_original == 0xcc || !maro_Arm(maro_process, maro_breakpoint))
                        {
                            maro_fail(GetLastError(), L"소스 행 중단점을 설정할 수 없습니다.");
                            break;
                        }
                    }
                }
                break;
            }
            case CREATE_THREAD_DEBUG_EVENT:
                maro_threads.emplace(maro_event.dwThreadId, maro_event.u.CreateThread.hThread);
                break;
            case EXIT_THREAD_DEBUG_EVENT:
                maro_threads.erase(maro_event.dwThreadId);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (maro_event.u.LoadDll.hFile) CloseHandle(maro_event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT:
            {
                maro_continue = DBG_EXCEPTION_NOT_HANDLED;
                if (maro_stopping) break;
                const auto& maro_exception = maro_event.u.Exception.ExceptionRecord;
                const DWORD64 maro_address = reinterpret_cast<DWORD64>(maro_exception.ExceptionAddress);
                const auto maro_threadIt = maro_threads.find(maro_event.dwThreadId);
                const auto maro_point = maro_breakpoints.find(maro_address);
                if (maro_exception.ExceptionCode == EXCEPTION_BREAKPOINT &&
                    maro_point != maro_breakpoints.end() && maro_point->second.maro_armed &&
                    maro_threadIt != maro_threads.end())
                {
                    const HANDLE maro_thread = maro_threadIt->second;
                    CONTEXT maro_context{};
                    maro_context.ContextFlags = CONTEXT_FULL;
                    if (!GetThreadContext(maro_thread, &maro_context) || !maro_Disarm(maro_process, maro_point->second))
                    {
                        maro_fail(GetLastError(), L"중단된 스레드 상태를 읽을 수 없습니다.");
                        break;
                    }
                    maro_context.Rip = maro_address;
                    auto maro_action = maro_TraceAction::maro_continue;
                    if (!maro_session->maro_IsContinuing())
                        maro_action = maro_session->maro_Pause(maro_Capture(maro_process,
                            maro_thread, maro_context, maro_source, maro_point->second.maro_line), maro_cancelled);
                    if (maro_action == maro_TraceAction::maro_cancel)
                    {
                        maro_result.maro_cancelled = true;
                        maro_stopping = true;
                        maro_stopStarted = GetTickCount64();
                        TerminateProcess(maro_process, ERROR_CANCELLED);
                    }
                    else if (maro_action == maro_TraceAction::maro_continue)
                    {
                        for (auto& [maro_key, maro_breakpoint] : maro_breakpoints)
                        {
                            (void)maro_key;
                            if (!maro_Disarm(maro_process, maro_breakpoint))
                                maro_fail(GetLastError(), L"중단점을 해제할 수 없습니다.");
                        }
                    }
                    else
                    {
                        for (const auto& [maro_id, maro_other] : maro_threads)
                            if (maro_id != maro_event.dwThreadId && SuspendThread(maro_other) != static_cast<DWORD>(-1))
                                maro_suspended.push_back(maro_other);
                        maro_context.EFlags |= 0x100;
                        maro_rearm = maro_address;
                        maro_stepThread = maro_event.dwThreadId;
                    }
                    if (!maro_stopping && !SetThreadContext(maro_thread, &maro_context))
                        maro_fail(GetLastError(), L"한 줄 실행 상태를 적용할 수 없습니다.");
                    maro_continue = DBG_CONTINUE;
                }
                else if (maro_exception.ExceptionCode == EXCEPTION_SINGLE_STEP && maro_rearm &&
                    maro_event.dwThreadId == maro_stepThread && maro_threadIt != maro_threads.end())
                {
                    const auto maro_rearmIt = maro_breakpoints.find(maro_rearm);
                    if (maro_rearmIt != maro_breakpoints.end() && !maro_Arm(maro_process, maro_rearmIt->second))
                        maro_fail(GetLastError(), L"반복 실행 중단점을 복원할 수 없습니다.");
                    CONTEXT maro_context{};
                    maro_context.ContextFlags = CONTEXT_CONTROL;
                    if (!GetThreadContext(maro_threadIt->second, &maro_context))
                        maro_fail(GetLastError(), L"한 단계 실행 상태를 읽을 수 없습니다.");
                    else
                    {
                        maro_context.EFlags &= ~DWORD{0x100};
                        if (!SetThreadContext(maro_threadIt->second, &maro_context))
                            maro_fail(GetLastError(), L"한 단계 실행 상태를 복원할 수 없습니다.");
                    }
                    maro_rearm = 0;
                    maro_stepThread = 0;
                    maro_resumeOthers();
                    maro_continue = DBG_CONTINUE;
                }
                else if (maro_exception.ExceptionCode == EXCEPTION_BREAKPOINT && !maro_initialBreakpoint)
                {
                    maro_initialBreakpoint = true;
                    maro_continue = DBG_CONTINUE;
                }
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                maro_result.maro_exited = true;
                maro_result.maro_exitCode = maro_event.u.ExitProcess.dwExitCode;
                maro_exit = true;
                break;
            default: break;
            }
        }
        catch (...)
        {
            maro_fail(ERROR_NOT_ENOUGH_MEMORY, L"시각화 상태를 수집할 수 없습니다.");
        }
        if (!ContinueDebugEvent(maro_event.dwProcessId, maro_event.dwThreadId, maro_continue))
            maro_fail(GetLastError(), L"네이티브 실행을 계속할 수 없습니다.");
        if (maro_exit) break;
    }
    maro_resumeOthers();
    if (maro_symbolsReady)
    {
        std::lock_guard maro_lock(maro_symbolsMutex);
        SymCleanup(maro_process);
    }
    if (maro_session) maro_session->maro_Finish();
    return maro_result;
}
