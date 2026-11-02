#include "Log.h"

namespace netmic {

os_log_t driverLog() {
    static os_log_t log = os_log_create("com.netmic.driver", "driver");
    return log;
}

os_log_t asplLog() {
    static os_log_t log = os_log_create("com.netmic.driver", "aspl");
    return log;
}

}  // namespace netmic
