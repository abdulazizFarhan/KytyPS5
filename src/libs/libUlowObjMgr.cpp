#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("UlowObjMgr", 1, "UlowObjMgr", 1, 1);

namespace UlowObjMgr {

// GTA V ulobjmgr_v1 cycle 0141bc: BG26hBGiNlw stub (returns 0)
// Called from GTA V's launcher_init wrapper (PLT 4) and libSceJobManager.prx
static KYTY_SYSV_ABI int BG26hBGiNlw_stub() {
	PRINT_NAME();
	return 0;
}

// GTA V ulobjmgr_v1 cycle 0141bc: Smf+fUNblPc stub (returns 0)
// Called from GTA V's main() (PLT 6) and libSceJobManager.prx
static KYTY_SYSV_ABI int Smf_fUNblPc_stub() {
	PRINT_NAME();
	return 0;
}

} // namespace UlowObjMgr

LIB_DEFINE(InitUlowObjMgr_1) {
	LIB_FUNC("BG26hBGiNlw", UlowObjMgr::BG26hBGiNlw_stub);
	LIB_FUNC("Smf+fUNblPc", UlowObjMgr::Smf_fUNblPc_stub);
}

} // namespace Libs
