// SPDX-License-Identifier: MIT
// AR-4 link probe: settimeofday must reach the validated library store.
// Built with the existing firmware probes; no board or network is required
// to inspect the resolved setter in the map and symbol table.
#include <sys/time.h>

#include <cerrno>
#include <pico_time/time_sync/time_sync.hpp>

int main() {
    // Reference the library clock as a firmware consumer does.
    if (pico_time::is_time_synchronized()) {
        return 1;
    }
    errno = 0;
    return settimeofday(nullptr, nullptr) == -1 && errno == EINVAL ? 0 : 1;
}
