#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <cstdio>
#endif

// CPU stack effects are simulated independently; the operating system consumes
// the generated UNWIND_INFO at every instruction boundary and both jump sites.
namespace {
enum class operation { push, allocate, anchor_rsp, anchor_r11, save_rsp, save_rax, call, jump };
struct instruction {
    std::size_t offset{}, size{};
    operation action{};
    unsigned reg{};
    int displacement{};
};

constexpr std::array<unsigned, 8> nonvolatile{3, 5, 6, 7, 12, 13, 14, 15};
constexpr DWORD64 poison = 0xdddd000000000000ull;
// Twenty distinct instruction sequences from the eighty Java 8-27 entries.
constexpr std::byte prefix_0[]{std::byte{0x48},std::byte{0x89},std::byte{0x5c},std::byte{0x24},std::byte{0x18}};
constexpr std::byte prefix_1[]{std::byte{0x4c},std::byte{0x8b},std::byte{0xdc},std::byte{0x55},std::byte{0x48},std::byte{0x81},std::byte{0xec},std::byte{0x30},std::byte{0x02},std::byte{0x00},std::byte{0x00}};
constexpr std::byte prefix_2[]{std::byte{0x48},std::byte{0x89},std::byte{0x5c},std::byte{0x24},std::byte{0x08}};
constexpr std::byte prefix_3[]{std::byte{0xe9},std::byte{0x3b},std::byte{0x11},std::byte{0x03},std::byte{0x00}};
constexpr std::byte prefix_4[]{std::byte{0x48},std::byte{0x89},std::byte{0x4c},std::byte{0x24},std::byte{0x08}};
constexpr std::byte prefix_5[]{std::byte{0x48},std::byte{0x83},std::byte{0xec},std::byte{0x28},std::byte{0xe8},std::byte{0x07},std::byte{0x30},std::byte{0x38},std::byte{0x00}};
constexpr std::byte prefix_6[]{std::byte{0x40},std::byte{0x55},std::byte{0x53},std::byte{0x56},std::byte{0x57}};
constexpr std::byte prefix_7[]{std::byte{0x48},std::byte{0x8b},std::byte{0xc4},std::byte{0x4c},std::byte{0x89},std::byte{0x48},std::byte{0x20}};
constexpr std::byte prefix_8[]{std::byte{0x48},std::byte{0x89},std::byte{0x5c},std::byte{0x24},std::byte{0x10}};
constexpr std::byte prefix_9[]{std::byte{0x48},std::byte{0x8b},std::byte{0xc4},std::byte{0x48},std::byte{0x89},std::byte{0x58},std::byte{0x10}};
constexpr std::byte prefix_10[]{std::byte{0x40},std::byte{0x55},std::byte{0x56},std::byte{0x57},std::byte{0x41},std::byte{0x55}};
constexpr std::byte prefix_11[]{std::byte{0x40},std::byte{0x53},std::byte{0x41},std::byte{0x55},std::byte{0x41},std::byte{0x57}};
constexpr std::byte prefix_12[]{std::byte{0x48},std::byte{0x89},std::byte{0x74},std::byte{0x24},std::byte{0x20}};
constexpr std::byte prefix_13[]{std::byte{0x40},std::byte{0x55},std::byte{0x56},std::byte{0x57},std::byte{0x41},std::byte{0x54}};
constexpr std::byte prefix_14[]{std::byte{0x48},std::byte{0x8b},std::byte{0xc4},std::byte{0x48},std::byte{0x89},std::byte{0x58},std::byte{0x18}};
constexpr std::byte prefix_15[]{std::byte{0x40},std::byte{0x55},std::byte{0x56},std::byte{0x57},std::byte{0x41},std::byte{0x56}};
constexpr std::byte prefix_16[]{std::byte{0x48},std::byte{0x8b},std::byte{0xc4},std::byte{0x48},std::byte{0x89},std::byte{0x58},std::byte{0x20}};
constexpr std::byte prefix_17[]{std::byte{0x40},std::byte{0x53},std::byte{0x55},std::byte{0x41},std::byte{0x57}};
constexpr std::byte prefix_18[]{std::byte{0x40},std::byte{0x53},std::byte{0x41},std::byte{0x56},std::byte{0x41},std::byte{0x57}};
constexpr std::byte prefix_19[]{std::byte{0x40},std::byte{0x53},std::byte{0x55},std::byte{0x56},std::byte{0x48},std::byte{0x83},std::byte{0xec},std::byte{0x40}};
constexpr std::array<std::span<const std::byte>, 20> prefixes{
    prefix_0, prefix_1, prefix_2, prefix_3, prefix_4, prefix_5, prefix_6, prefix_7, prefix_8, prefix_9, prefix_10, prefix_11, prefix_12, prefix_13, prefix_14, prefix_15, prefix_16, prefix_17, prefix_18, prefix_19
};

auto reg(CONTEXT& context, unsigned index) -> DWORD64& {
    switch (index) {
    case 0: return context.Rax;
    case 1: return context.Rcx;
    case 2: return context.Rdx;
    case 3: return context.Rbx;
    case 4: return context.Rsp;
    case 5: return context.Rbp;
    case 6: return context.Rsi;
    case 7: return context.Rdi;
    case 8: return context.R8;
    case 9: return context.R9;
    case 10: return context.R10;
    case 11: return context.R11;
    case 12: return context.R12;
    case 13: return context.R13;
    case 14: return context.R14;
    default: return context.R15;
    }
}

auto original_value(unsigned index) -> DWORD64 {
    return 0x1111000000000000ull | (static_cast<DWORD64>(index) << 32) | index;
}

// Decode the exact profile instructions independently of UNWIND_INFO. The
// simulator models CPU stack effects; RtlVirtualUnwind consumes hook metadata.
auto decode(std::span<const std::byte> bytes, std::vector<instruction>& output) -> bool {
    const auto byte = [&](std::size_t i) { return std::to_integer<unsigned>(bytes[i]); };
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto left = bytes.size() - offset;
        if (left >= 3 && byte(offset) == 0x48 && byte(offset + 1) == 0x8b && byte(offset + 2) == 0xc4) {
            output.push_back({offset, 3, operation::anchor_rsp});
            offset += 3;
        } else if (left >= 3 && byte(offset) == 0x4c && byte(offset + 1) == 0x8b && byte(offset + 2) == 0xdc) {
            output.push_back({offset, 3, operation::anchor_r11});
            offset += 3;
        } else if (left >= 4 && byte(offset) == 0x48 && byte(offset + 1) == 0x83 && byte(offset + 2) == 0xec) {
            output.push_back({offset, 4, operation::allocate, 0, static_cast<int>(byte(offset + 3))});
            offset += 4;
        } else if (left >= 7 && byte(offset) == 0x48 && byte(offset + 1) == 0x81 && byte(offset + 2) == 0xec) {
            std::int32_t amount{};
            std::memcpy(&amount, bytes.data() + offset + 3, sizeof amount);
            output.push_back({offset, 7, operation::allocate, 0, amount});
            offset += 7;
        } else if (left >= 5 && (byte(offset) == 0x48 || byte(offset) == 0x4c) && byte(offset + 1) == 0x89
                   && (byte(offset + 2) & 0xc7) == 0x44 && byte(offset + 3) == 0x24) {
            output.push_back({offset, 5, operation::save_rsp, ((byte(offset + 2) >> 3) & 7) + (byte(offset) == 0x4c ? 8u : 0u),
                              static_cast<std::int8_t>(byte(offset + 4))});
            offset += 5;
        } else if (left >= 4 && (byte(offset) == 0x48 || byte(offset) == 0x4c) && byte(offset + 1) == 0x89
                   && (byte(offset + 2) & 0xc7) == 0x40) {
            output.push_back({offset, 4, operation::save_rax, ((byte(offset + 2) >> 3) & 7) + (byte(offset) == 0x4c ? 8u : 0u),
                              static_cast<std::int8_t>(byte(offset + 3))});
            offset += 4;
        } else if (left >= 5 && (byte(offset) == 0xe8 || byte(offset) == 0xe9)) {
            std::int32_t displacement{};
            std::memcpy(&displacement, bytes.data() + offset + 1, sizeof displacement);
            output.push_back({offset, 5, byte(offset) == 0xe8 ? operation::call : operation::jump, 0, displacement});
            offset += 5;
        } else {
            unsigned extension{};
            auto length = std::size_t{1};
            if (byte(offset) == 0x40 || byte(offset) == 0x41) {
                if (left < 2) return false;
                extension = byte(offset) == 0x41 ? 8 : 0;
                length = 2;
            }
            const auto opcode = byte(offset + length - 1);
            if (opcode < 0x50 || opcode > 0x57) return false;
            output.push_back({offset, length, operation::push, extension + opcode - 0x50});
            offset += length;
        }
    }
    return true;
}

auto execute(const instruction& ins, CONTEXT& context, unsigned& saved_mask) -> void {
    switch (ins.action) {
    case operation::push:
        context.Rsp -= 8;
        *reinterpret_cast<DWORD64*>(context.Rsp) = reg(context, ins.reg);
        saved_mask |= 1u << ins.reg;
        break;
    case operation::allocate:
        context.Rsp -= ins.displacement;
        break;
    case operation::anchor_rsp:
        context.Rax = context.Rsp;
        break;
    case operation::anchor_r11:
        context.R11 = context.Rsp;
        break;
    case operation::save_rsp:
    case operation::save_rax:
        *reinterpret_cast<DWORD64*>((ins.action == operation::save_rsp ? context.Rsp : context.Rax)
                                   + ins.displacement) = reg(context, ins.reg);
        saved_mask |= 1u << ins.reg;
        break;
    case operation::call:
    case operation::jump:
        // The synthetic callee returns without changing nonvolatile registers;
        // CALL followed by its RET has no net stack effect at the next boundary.
        break;
    }
}

auto append_safe_epilogue(std::byte* destination, const std::vector<instruction>& instructions) -> void {
    std::size_t cursor{};
    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
        if (it->action == operation::allocate) {
            destination[cursor++] = std::byte{0x48};
            destination[cursor++] = it->displacement <= 127 ? std::byte{0x83} : std::byte{0x81};
            destination[cursor++] = std::byte{0xc4};
            if (it->displacement <= 127) destination[cursor++] = static_cast<std::byte>(it->displacement);
            else {
                std::memcpy(destination + cursor, &it->displacement, 4);
                cursor += 4;
            }
        } else if (it->action == operation::push) {
            if (it->reg >= 8) destination[cursor++] = std::byte{0x41};
            destination[cursor++] = static_cast<std::byte>(0x58 + (it->reg & 7));
        }
    }
    destination[cursor] = std::byte{0xc3};
}

auto return_marker() -> void {}
auto detour() -> void {}

auto unwind_at(std::byte* code, std::size_t offset, CONTEXT context, unsigned saved_mask,
               DWORD64 entry_rsp, DWORD64 return_address, int feature, std::size_t hook,
               bool expect_pass = true, bool report_failure = true) -> bool {
    // Make restoration observable even for stores which do not themselves
    // change a nonvolatile register. Only already-saved registers are poisoned.
    for (const auto index : nonvolatile) if (saved_mask & (1u << index)) reg(context, index) = poison | index;
    context.Rip = reinterpret_cast<DWORD64>(code + offset);
    DWORD64 image_base{};
    auto* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function || image_base != reinterpret_cast<DWORD64>(code)) {
        std::printf("feature=%d hook=%zu pc=%zu function_lookup=fail\n", feature, hook, offset);
        return false;
    }
    PVOID handler_data{};
    DWORD64 establisher{};
    KNONVOLATILE_CONTEXT_POINTERS pointers{};
    const auto handler = RtlVirtualUnwind(0, image_base, context.Rip, function,
                                         &context, &handler_data, &establisher, &pointers);
    bool passed = handler == nullptr && context.Rip == return_address && context.Rsp == entry_rsp + 8;
    for (const auto index : nonvolatile) passed &= reg(context, index) == original_value(index);
    if (passed != expect_pass) {
        if (!report_failure) return false;
        std::printf("feature=%d hook=%zu pc=%zu unwind=fail rip=%llx rsp=%llx"
                    " expected_rip=%llx expected_rsp=%llx\n", feature, hook, offset,
                    static_cast<unsigned long long>(context.Rip), static_cast<unsigned long long>(context.Rsp),
                    static_cast<unsigned long long>(return_address), static_cast<unsigned long long>(entry_rsp + 8));
        for (const auto index : nonvolatile) if (reg(context, index) != original_value(index))
            std::printf("register=%u actual=%llx expected=%llx\n", index,
                        static_cast<unsigned long long>(reg(context, index)), static_cast<unsigned long long>(original_value(index)));
        return false;
    }
    return true;
}
}

auto main() -> int {
    std::size_t profiles{}, boundary_checks{}, relay_checks{}, negative_controls{}, ff25_detected{};
    std::size_t relocated_call_checks{}, relocated_jump_checks{}, anchor_execution_checks{}, continuation_encoding_checks{};
    struct fixture { int feature; std::size_t index; std::span<const std::byte> prefix; };
    std::vector<fixture> fixtures;
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
        fixtures.push_back({0, index, prefixes[index]});
    }
    bool all_passed = true;
    for (const auto& profile : fixtures) {
            const auto index = profile.index;
            const auto prefix = profile.prefix;
            std::vector<instruction> instructions;
            if (!decode(prefix, instructions)) {
                std::printf("feature=%d hook=%zu prefix_decode=fail\n", profile.feature, index);
                all_passed = false;
                continue;
            }
            auto allocation_size = std::size_t{4096};
            for (const auto& ins : instructions) if (ins.action == operation::call || ins.action == operation::jump)
                allocation_size = std::max(allocation_size, static_cast<std::size_t>(ins.offset + ins.size + ins.displacement) + 4096);
            auto* target = static_cast<std::byte*>(VirtualAlloc(nullptr, allocation_size, vmhook::windows::mem_reserve | vmhook::windows::mem_commit, vmhook::windows::page_execute_readwrite));
            if (!target) return 2;
            std::memcpy(target, prefix.data(), prefix.size());
            const auto has_r11_anchor = std::any_of(instructions.begin(), instructions.end(), [](const auto& ins) { return ins.action == operation::anchor_r11; });
            auto* resume = target + prefix.size();
            if (has_r11_anchor) {
                std::int32_t stack_size{};
                for (const auto& ins : instructions) {
                    if (ins.action == operation::allocate) stack_size += ins.displacement;
                    if (ins.action == operation::push) stack_size += 8;
                }
                // Return whether R11 still equals the CPU's entry RSP. This
                // exercises continuation register selection with the real CPU.
                constexpr std::array<unsigned char, 17> compare_anchor{
                    0x48, 0x8d, 0x84, 0x24, 0, 0, 0, 0, // lea rax,[rsp+stack_size]
                    0x4c, 0x39, 0xd8,                   // cmp rax,r11
                    0x0f, 0x94, 0xc0,                   // sete al
                    0x0f, 0xb6, 0xc0};                  // movzx eax,al
                std::memcpy(resume, compare_anchor.data(), compare_anchor.size());
                std::memcpy(resume + 4, &stack_size, sizeof stack_size);
                resume += compare_anchor.size();
            }
            append_safe_epilogue(resume, instructions);
            constexpr std::uint32_t callee_result = 0xc411ee;
            for (const auto& ins : instructions) if (ins.action == operation::call || ins.action == operation::jump) {
                auto* callee = target + ins.offset + ins.size + ins.displacement;
                callee[0] = std::byte{0xb8}; // mov eax, sentinel; ret
                std::memcpy(callee + 1, &callee_result, sizeof callee_result);
                callee[5] = std::byte{0xc3};
            }
            if (!FlushInstructionCache(GetCurrentProcess(), target, allocation_size)) return 3;
            vmhook::detail::class_events::native_entry_hook hook;
            if (!hook.install(target, reinterpret_cast<const void*>(&detour), prefix) || !hook.matches()) {
                std::printf("feature=%d hook=%zu install=fail error=%lu\n", profile.feature, index, GetLastError());
                all_passed = false;
                continue;
            }
            ++profiles;
            auto* code = static_cast<std::byte*>(hook.original());
            alignas(16) std::array<DWORD64, 256> stack{};
            const auto entry_rsp = reinterpret_cast<DWORD64>(stack.data() + 191);
            const auto return_address = reinterpret_cast<DWORD64>(&return_marker);
            *reinterpret_cast<DWORD64*>(entry_rsp) = return_address;
            CONTEXT context{};
            context.ContextFlags = 0x10000bu;
            context.Rsp = entry_rsp;
            context.Rax = original_value(0);
            for (const auto reg_index : nonvolatile) reg(context, reg_index) = original_value(reg_index);
            unsigned saved_mask{};
            bool passed = true;
            const auto continuation_ok = code[prefix.size()] == std::byte{0x49}
                && code[prefix.size() + 1] == std::byte{0xba}
                && code[prefix.size() + 10] == std::byte{0x41}
                && code[prefix.size() + 11] == std::byte{0xff}
                && code[prefix.size() + 12] == std::byte{0xe2};
            passed &= continuation_ok;
            ++continuation_encoding_checks;
            if (has_r11_anchor) {
                const auto returned = reinterpret_cast<std::uint64_t(*)()>(code)();
                passed &= returned == 1;
                ++anchor_execution_checks;
                std::printf("feature=%d hook=%zu r11_anchor_execution=%s continuation_r10=%s\n", profile.feature, index,
                            returned == 1 ? "pass" : "fail", continuation_ok ? "pass" : "fail");
            }
            for (const auto& ins : instructions) if (ins.action == operation::call || ins.action == operation::jump) {
                std::int32_t moved{};
                std::memcpy(&moved, code + ins.offset + 1, sizeof moved);
                const auto expected_callee = target + ins.offset + ins.size + ins.displacement;
                passed &= code + ins.offset + ins.size + moved == expected_callee;
                const auto returned = reinterpret_cast<std::uint64_t(*)()>(code)();
                passed &= returned == callee_result;
                if (ins.action == operation::call) ++relocated_call_checks;
                else ++relocated_jump_checks;
                std::printf("feature=%d hook=%zu %s_relocation=%s %s_execution=%s\n", profile.feature, index,
                            ins.action == operation::call ? "call" : "tail_jump",
                            code + ins.offset + ins.size + moved == expected_callee ? "pass" : "fail",
                            ins.action == operation::call ? "call" : "tail_jump",
                            returned == callee_result ? "pass" : "fail");
            }
            for (const auto& ins : instructions) {
                passed &= unwind_at(code, ins.offset, context, saved_mask, entry_rsp, return_address, profile.feature, index);
                ++boundary_checks;
                execute(ins, context, saved_mask);
            }
            passed &= unwind_at(code, prefix.size(), context, saved_mask, entry_rsp, return_address, profile.feature, index);
            ++boundary_checks;
            context.R10 = reinterpret_cast<DWORD64>(target + prefix.size());
            passed &= unwind_at(code, prefix.size() + 10, context, saved_mask, entry_rsp, return_address, profile.feature, index);
            ++boundary_checks;
            CONTEXT relay_context{};
            relay_context.ContextFlags = 0x10000bu;
            relay_context.Rsp = entry_rsp;
            for (const auto reg_index : nonvolatile) reg(relay_context, reg_index) = original_value(reg_index);
            passed &= unwind_at(code, 32, relay_context, 0, entry_rsp, return_address, profile.feature, index);
            ++relay_checks;

            // Reproduce epilogue-shaped continuations only in this allocated
            // test trampoline. Wine 10 omits FF /4 epilogue recognition, so that
            // observation is reported separately. The RET control is recognized
            // by Wine 10 and Windows and must expose the incorrect unwind.
            if (negative_controls == 0 && saved_mask && context.Rsp != entry_rsp) {
                DWORD old{};
                if (!VirtualProtect(code, 4096, vmhook::windows::page_execute_readwrite, &old)) return 4;
                std::array<std::byte, 14> old_bytes{};
                std::memcpy(old_bytes.data(), code + prefix.size(), old_bytes.size());
                constexpr std::array<unsigned char, 6> previous_jump{0xff, 0x25, 0, 0, 0, 0};
                std::memcpy(code + prefix.size(), previous_jump.data(), previous_jump.size());
                const auto destination = target + prefix.size();
                std::memcpy(code + prefix.size() + 6, &destination, 8);
                if (!FlushInstructionCache(GetCurrentProcess(), code, 4096)) return 5;
                ff25_detected = unwind_at(code, prefix.size(), context, saved_mask, entry_rsp, return_address,
                                           profile.feature, index, false, false) ? 1 : 0;
                std::printf("old_ff25_epilogue_recognition=%s\n", ff25_detected ? "detected" : "absent_in_this_unwind_engine");
                code[prefix.size()] = std::byte{0xc3};
                if (!FlushInstructionCache(GetCurrentProcess(), code, 4096)) return 8;
                const auto caught = unwind_at(code, prefix.size(), context, saved_mask, entry_rsp, return_address,
                                              profile.feature, index, false);
                passed &= caught;
                std::printf("ret_epilogue_negative_control=%s\n", caught ? "pass" : "fail");
                ++negative_controls;
                std::memcpy(code + prefix.size(), old_bytes.data(), old_bytes.size());
                if (!FlushInstructionCache(GetCurrentProcess(), code, 4096)) return 6;
                DWORD ignored{};
                if (!VirtualProtect(code, 4096, old, &ignored)) return 7;
            }
            std::printf("feature=%d hook=%zu prefix_bytes=%zu prefix_instructions=%zu unwind=%s\n",
                        profile.feature, index, prefix.size(), instructions.size(), passed ? "pass" : "fail");
            all_passed &= passed;
            // Entry patches, code pages, and dynamic function tables deliberately
            // remain resident for the process lifetime, matching the hook API.
    }
    all_passed &= profiles == fixtures.size() && negative_controls == 1 && relocated_call_checks == 1
        && relocated_jump_checks == 1 && anchor_execution_checks == 1;
    std::printf("profiles=%zu instruction_boundary_checks=%zu relay_checks=%zu negative_controls=%zu relocated_call_checks=%zu relocated_jump_checks=%zu r11_anchor_checks=%zu continuation_r10_checks=%zu\n",
                profiles, boundary_checks, relay_checks, negative_controls, relocated_call_checks,
                relocated_jump_checks, anchor_execution_checks, continuation_encoding_checks);
    std::printf("synthetic_unwind=%s\n", all_passed ? "pass" : "fail");
    return all_passed ? 0 : 1;
}
