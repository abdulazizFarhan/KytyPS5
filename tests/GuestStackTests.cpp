#include "kernel/pthread.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr uintptr_t kSentinel = 0x12345678u;

// Sink the guest writes to (host address) — declared volatile so the
// compiler must commit it to memory and emit a real load on read.
volatile uintptr_t g_observed = 0;

} // namespace

extern "C" KYTY_SYSV_ABI void* ClobberHostCarrierRegisters(void* /*arg*/) {
	g_observed = kSentinel;
#if defined(__x86_64__) || defined(_M_X64)
	// Deliberately clobber the callee-saved registers the previous
	// RunOnGuestStack implementation relied on for host-state restore.
	asm volatile("movq $0x11111111, %%r12\n\t"
	             "movq $0x22222222, %%r13\n\t"
	             :
	             :
	             : "r12", "r13", "memory");
#endif
	return reinterpret_cast<void*>(kSentinel);
}

int main() {
	std::vector<uint8_t> guest_stack(0x10000);
	void* const stack_top = guest_stack.data() + guest_stack.size();

	std::printf("guest_stack_tests: stack_top=%p result=in\n", stack_top);
	void* const result = Libs::LibKernel::PthreadRunOnGuestStackForTest(
	    nullptr, ClobberHostCarrierRegisters, stack_top);
	std::printf("guest_stack_tests: returned %p observed=0x%zx\n", result,
	             static_cast<size_t>(g_observed));
	if (g_observed != kSentinel) {
		std::fprintf(stderr, "guest_stack_tests: host frame did not survive (sentinel = 0x%zx)\n",
		             static_cast<size_t>(g_observed));
		return 1;
	}
	if (reinterpret_cast<uintptr_t>(result) != kSentinel) {
		std::fprintf(stderr, "guest_stack_tests: unexpected return value %p\n", result);
		return 1;
	}
	std::printf("guest_stack_tests: host stack restored after guest register clobber\n");
	return 0;
}
