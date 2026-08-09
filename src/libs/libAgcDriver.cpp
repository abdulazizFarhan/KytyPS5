#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("AgcDriver", 1, "AgcDriver", 1, 1);

namespace AgcDriver {

// GTA V agc_driver_v1 cycle 0141bj: +TN0oRTBxJQ stub (returns 0)
static KYTY_SYSV_ABI int _TN0oRTBxJQ_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: 5l3IfCFJxBs stub (returns 0)
static KYTY_SYSV_ABI int _5l3IfCFJxBs_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: FOwvmNlFLjM stub (returns 0)
static KYTY_SYSV_ABI int FOwvmNlFLjM_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: LepGrgk77sM stub (returns 0)
static KYTY_SYSV_ABI int LepGrgk77sM_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: M9yBzRKkjPc stub (returns 0)
static KYTY_SYSV_ABI int M9yBzRKkjPc_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: NghWEUXp1qM stub (returns 0)
static KYTY_SYSV_ABI int NghWEUXp1qM_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: SAfhzJPcjuk stub (returns 0)
static KYTY_SYSV_ABI int SAfhzJPcjuk_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: SCoAN5fYlUM stub (returns 0)
static KYTY_SYSV_ABI int SCoAN5fYlUM_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: VOMSpd9+vxU stub (returns 0)
static KYTY_SYSV_ABI int VOMSpd9_vxU_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: Xq5WmbwPTnQ stub (returns 0)
static KYTY_SYSV_ABI int Xq5WmbwPTnQ_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: ZLJk9r2+2Aw stub (returns 0)
static KYTY_SYSV_ABI int ZLJk9r2_2Aw_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: emP3ckeS2uo stub (returns 0)
static KYTY_SYSV_ABI int emP3ckeS2uo_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: ls4jfY576lw stub (returns 0)
static KYTY_SYSV_ABI int ls4jfY576lw_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: mXn+K9E-wOA stub (returns 0)
static KYTY_SYSV_ABI int mXn_K9E_wOA_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: n5ElQVYsU1A stub (returns 0)
static KYTY_SYSV_ABI int n5ElQVYsU1A_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: qspAL8bgcBY stub (returns 0)
static KYTY_SYSV_ABI int qspAL8bgcBY_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: rI9lNAXPMIw stub (returns 0)
static KYTY_SYSV_ABI int rI9lNAXPMIw_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V agc_driver_v1 cycle 0141bj: rJUyMrDdxJg stub (returns 0)
static KYTY_SYSV_ABI int rJUyMrDdxJg_stub() {
	PRINT_NAME();
	return 0;
}

} // namespace AgcDriver

LIB_DEFINE(InitAgcDriver_1) {
	LIB_FUNC("+TN0oRTBxJQ", AgcDriver::_TN0oRTBxJQ_stub);
	LIB_FUNC("5l3IfCFJxBs", AgcDriver::_5l3IfCFJxBs_stub);
	LIB_FUNC("FOwvmNlFLjM", AgcDriver::FOwvmNlFLjM_stub);
	LIB_FUNC("LepGrgk77sM", AgcDriver::LepGrgk77sM_stub);
	LIB_FUNC("M9yBzRKkjPc", AgcDriver::M9yBzRKkjPc_stub);
	LIB_FUNC("NghWEUXp1qM", AgcDriver::NghWEUXp1qM_stub);
	LIB_FUNC("SAfhzJPcjuk", AgcDriver::SAfhzJPcjuk_stub);
	LIB_FUNC("SCoAN5fYlUM", AgcDriver::SCoAN5fYlUM_stub);
	LIB_FUNC("VOMSpd9+vxU", AgcDriver::VOMSpd9_vxU_stub);
	LIB_FUNC("Xq5WmbwPTnQ", AgcDriver::Xq5WmbwPTnQ_stub);
	LIB_FUNC("ZLJk9r2+2Aw", AgcDriver::ZLJk9r2_2Aw_stub);
	LIB_FUNC("emP3ckeS2uo", AgcDriver::emP3ckeS2uo_stub);
	LIB_FUNC("ls4jfY576lw", AgcDriver::ls4jfY576lw_stub);
	LIB_FUNC("mXn+K9E-wOA", AgcDriver::mXn_K9E_wOA_stub);
	LIB_FUNC("n5ElQVYsU1A", AgcDriver::n5ElQVYsU1A_stub);
	LIB_FUNC("qspAL8bgcBY", AgcDriver::qspAL8bgcBY_stub);
	LIB_FUNC("rI9lNAXPMIw", AgcDriver::rI9lNAXPMIw_stub);
	LIB_FUNC("rJUyMrDdxJg", AgcDriver::rJUyMrDdxJg_stub);
}

} // namespace Libs