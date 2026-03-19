/* ESP-IDF entry point that runs an embedded .xsc bytecode blob.
 * Build the .xsc on the host with `xs build app.xs -o app.xsc` and
 * register it as a binary file in this component's CMakeLists.txt:
 *   target_add_binary_data(${COMPONENT_LIB} "app.xsc" BINARY)
 * Then link against components/xs/libxs.a built via `make esp32`.
 */
#include <stdio.h>
#include "esp_log.h"
#include "xs_embed.h"

extern const uint8_t app_bytecode_start[] asm("_binary_app_xsc_start");
extern const uint8_t app_bytecode_end[]   asm("_binary_app_xsc_end");

void app_main(void) {
    size_t size = (size_t)(app_bytecode_end - app_bytecode_start);
    ESP_LOGI("xs", "loading %u bytes of bytecode", (unsigned)size);
    int rc = xs_run_bytecode(NULL, app_bytecode_start, size);
    if (rc != 0) {
        ESP_LOGE("xs", "bytecode load failed (rc=%d)", rc);
    }
}
