#ifndef FOB_HOST_H
#define FOB_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the BLE layer to write an ACK/STATE frame to the fob's HOST
 * characteristic. The callback must not block. */
typedef void (*fob_host_tx_fn)(uint16_t conn_handle, uint16_t attr_handle,
                               const uint8_t *data, size_t len);

void fob_host_init(void);
void fob_host_set_tx(fob_host_tx_fn fn);
void fob_host_on_link_ready(uint16_t conn_handle, uint16_t host_handle);
void fob_host_on_link_down(void);
void fob_host_on_event(uint16_t conn_handle, const uint8_t *data, size_t len);
void fob_host_send_state(void);

#ifdef __cplusplus
}
#endif

#endif /* FOB_HOST_H */
