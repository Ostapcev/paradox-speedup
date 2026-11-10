#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
void* knowledge_original_target = nullptr;
void knowledge_shadow_hook(void* argument);
using InvokeFn = void* (*)(void* function, void* argument, void* current);
void* invoke_with_rdi(void* function, void* argument, void* current);
}

namespace {
int argument_value = 17;
int return_value = 29;
int current_value = 43;
void* seen_argument = nullptr;
void* seen_current = nullptr;
int verify_calls = 0;
int qpc_calls = 0;
int timing_calls = 0;
std::uint64_t expected_begin = 1000;
bool timing_after_original = false;

void* original_target(void* argument) {
    seen_argument = argument;
    return &return_value;
}

double original_double_target(void* argument) {
    seen_argument = argument;
    return 1234.5;
}
}

extern "C" std::uint64_t knowledge_qpc_now() noexcept {
    return 1000 + static_cast<std::uint64_t>(qpc_calls++);
}

extern "C" void knowledge_record_original(std::uint64_t begin) noexcept {
    if (begin != expected_begin) {
        std::fputs("wrong original timing begin\n", stderr);
        std::exit(3);
    }
    timing_after_original = seen_argument == &argument_value;
    ++timing_calls;
    ++expected_begin;
}

extern "C" void knowledge_shadow_verify(void* current) noexcept {
    seen_current = current;
    ++verify_calls;
}

int main() {
    knowledge_original_target = reinterpret_cast<void*>(&original_target);
    void* result = invoke_with_rdi(reinterpret_cast<void*>(&knowledge_shadow_hook),
        &argument_value, &current_value);
    if (result != &return_value || seen_argument != &argument_value ||
        seen_current != &current_value || verify_calls != 1) {
        std::fputs("integer wrapper preservation failed\n", stderr);
        return 1;
    }

    knowledge_original_target = reinterpret_cast<void*>(&original_double_target);
    using DoubleInvoke = double (*)(void*, void*, void*);
    const double floating = reinterpret_cast<DoubleInvoke>(invoke_with_rdi)(
        reinterpret_cast<void*>(&knowledge_shadow_hook), &argument_value, &current_value);
    if (floating != 1234.5 || verify_calls != 2) {
        std::fputs("XMM0 wrapper preservation failed\n", stderr);
        return 2;
    }
    std::puts("shadow wrapper tests passed: original arguments, RDI country, RAX and XMM0 returns");
    return 0;
}
