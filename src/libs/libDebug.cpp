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
// GTA V razor_v1 cycle 0141cx: KP+TBWGHlgs REAL implementation (sentinel 0xaa)
static KYTY_SYSV_ABI uint32_t KP_TBWGHlgs_impl() {
	PRINT_NAME();
	return 0xaa;
}

// GTA V razor_v1 cycle 0141cx: dnEdyY4+klQ REAL implementation (sentinel 0xbb)
static KYTY_SYSV_ABI uint32_t dnEdyY4_klQ_impl() {
	PRINT_NAME();
	return 0xbb;
}

// GTA V razor_v1 cycle 0141cx: 9FowWFMEIM8 REAL implementation (sentinel 0xcc)
static KYTY_SYSV_ABI uint32_t _9FowWFMEIM8_impl() {
	PRINT_NAME();
	return 0xcc;
}

LIB_DEFINE(InitLibRazorCpu_1) {

	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
	LIB_FUNC("KP+TBWGHlgs", LibRazorCpu::KP_TBWGHlgs_impl);
	LIB_FUNC("dnEdyY4+klQ", LibRazorCpu::dnEdyY4_klQ_impl);
	LIB_FUNC("9FowWFMEIM8", LibRazorCpu::_9FowWFMEIM8_impl);

}



} // namespace LibRazorCpu



LIB_DEFINE(InitDebug_1) {

	LibRazorCpu::InitLibRazorCpu_1(s);

}



} // namespace Libs

