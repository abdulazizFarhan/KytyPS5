#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace LibRazorCpu {

LIB_VERSION("RazorCpu", 1, "RazorCpu", 1, 1);

static KYTY_SYSV_ABI uint32_t RazorCpuIsCapturing() {
	PRINT_NAME();

	return 0;
}

// GTA V razor_v1 cycle 0141bj: 3 NID stubs from libSceJobManager.prx (returns 0)
static KYTY_SYSV_ABI uint32_t KP_TBWGHlgs_stub() {
	PRINT_NAME();
	return 0;
}

static KYTY_SYSV_ABI uint32_t dnEdyY4_klQ_stub() {
	PRINT_NAME();
	return 0;
}

static KYTY_SYSV_ABI uint32_t _9FowWFMEIM8_stub() {
	PRINT_NAME();
	return 0;
}

LIB_DEFINE(InitLibRazorCpu_1) {
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
	LIB_FUNC("KP+TBWGHlgs", LibRazorCpu::KP_TBWGHlgs_stub);
	LIB_FUNC("dnEdyY4+klQ", LibRazorCpu::dnEdyY4_klQ_stub);
	LIB_FUNC("9FowWFMEIM8", LibRazorCpu::_9FowWFMEIM8_stub);
}

} // namespace LibRazorCpu

LIB_DEFINE(InitDebug_1) {
	LibRazorCpu::InitLibRazorCpu_1(s);
}

} // namespace Libs
