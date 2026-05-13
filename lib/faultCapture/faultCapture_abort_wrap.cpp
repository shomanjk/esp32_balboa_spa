#if defined(DIAG_FAULT_CAPTURE)

#include "faultCapture.h"

extern "C" void __real_esp_system_abort(const char *details);

extern "C" void __wrap_esp_system_abort(const char *details)
{
  faultCaptureRecordEspSystemAbort(details);
  __real_esp_system_abort(details);
}

#else

static void fault_capture_abort_wrap_translation_unit_anchor(void) {}

#endif
