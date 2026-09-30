#pragma once

// h2o keeps the buffers and pool chunks a thread frees in caches that belong
// to that thread, and gives them back only when asked. A server's workers
// live as long as the process, so they never ask. A test thread that pumped
// a loop ends while the process goes on, and what it cached is then
// unreachable, which LeakSanitizer reports. Such a thread calls this last.

#include <h2o.h>

namespace owl_test {
    inline void release_h2o_thread_caches() {
        h2o_buffer_clear_recycle(1);
        h2o_mem_clear_recycle(&h2o_mem_pool_allocator, 1);
        h2o_mem_clear_recycle(&h2o_socket_ssl_buffer_allocator, 1);
        h2o_mem_clear_recycle(&h2o_socket_zerocopy_buffer_allocator, 1);
    }
}
