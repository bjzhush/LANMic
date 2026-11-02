// os_log under subsystem com.netmic.driver. Not real-time safe: never call
// from the IO thread.
#pragma once

#include <os/log.h>

namespace netmic {

os_log_t driverLog();  // category "driver"
os_log_t asplLog();    // category "aspl" (libASPL trace, debug level)

}  // namespace netmic

#define NM_LOG(fmt, ...) os_log(netmic::driverLog(), fmt, ##__VA_ARGS__)
#define NM_LOG_ERROR(fmt, ...) os_log_error(netmic::driverLog(), fmt, ##__VA_ARGS__)
