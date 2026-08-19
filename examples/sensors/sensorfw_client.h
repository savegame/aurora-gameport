/*
 * sensorfw_client.h -- read accelerometer/gyroscope directly from
 * sensorfwd, the Aurora OS sensor daemon.
 *
 * Plain-C API; the implementation (sensorfw_client.c) talks to
 * sensorfwd over D-Bus (session control) and a raw AF_UNIX socket
 * (sample stream) -- see docs/sensorfw.md for the protocol. No
 * engine-specific types leak into this header -- the module is
 * designed to be dropped into any project as-is.
 *
 * Reference: github.com/sailfishos/sensorfw. The session-control
 * D-Bus calls match that source; the on-wire sample layout does not
 * (see sensorfw_wire_xyz_t below and docs/sensorfw.md) -- re-verify
 * with sensorfw_rawdump.c before trusting either on a new build.
 *
 * Typical use:
 *   sensorfw_client_t *accel = sensorfw_client_open( "accelerometersensor" );
 *   sensorfw_client_set_interval_ms( accel, 20 );  // ~50 Hz
 *   sensorfw_client_start( accel );
 *   // in the main loop, once per frame:
 *   sensorfw_client_pump( accel, on_sample, NULL );
 *   // on shutdown:
 *   sensorfw_client_close( accel );
 *
 * Only sensors whose sensorfwd wire type matches sensorfw_wire_xyz_t
 * (timestamp + 3x int32 + 1 reserved int32, see sensorfw_client.c)
 * are supported: accelerometer and gyroscope are confirmed, both
 * D-Bus interface and data layout. Magnetometer / orientation /
 * rotation share the same struct upstream but neither their D-Bus
 * interface name nor their wire layout is verified here -- check
 * both with sensorfw_rawdump.c before trusting them.
 */

#ifndef SENSORFW_CLIENT_H
#define SENSORFW_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sensorfw_client sensorfw_client_t;

/*
 * One parsed sample. timestamp_us is sensorfwd's monotonic clock in
 * microseconds -- NOT wall-clock time and not synchronized to any
 * particular epoch; only useful for computing deltas between samples.
 *
 * x/y/z are passed through as float only for convenience -- sensorfwd
 * actually streams raw int32 driver counts, not SI units or
 * floating-point g (see docs/sensorfw.md and sensorfw_client.c). For
 * the accelerometer these look like milli-g (device flat -> z near
 * 1000, x/y near 0); treat any scaling as unverified until
 * cross-checked on the target device.
 */
typedef void (*sensorfw_sample_cb)( void *user_data, uint64_t timestamp_us,
                                     float x, float y, float z );

/*
 * Load the plugin, request a session and establish the data socket
 * for the given sensorfwd logical sensor id ("accelerometersensor",
 * "gyroscopesensor", ...). Returns NULL on any failure (system bus
 * unreachable, sensorfwd not running, unknown sensor id, socket
 * handshake failure) -- check stderr for the reason.
 */
sensorfw_client_t *sensorfw_client_open( const char *sensor_id );

/* Stop the session (if started), release it and close everything. */
void sensorfw_client_close( sensorfw_client_t *client );

/*
 * Request a sampling interval in milliseconds. Best-effort: sensord
 * may clamp to the nearest supported rate. Call before start() (can
 * also be called while running).
 */
bool sensorfw_client_set_interval_ms( sensorfw_client_t *client, int interval_ms );

/* Begin/stop the actual data flow into the socket. */
bool sensorfw_client_start( sensorfw_client_t *client );
bool sensorfw_client_stop( sensorfw_client_t *client );

/*
 * Underlying data-socket fd, for integrating into an existing
 * select()/poll() loop (becomes readable when a new frame arrives).
 * Do not read from it directly -- always go through pump().
 */
int sensorfw_client_fd( const sensorfw_client_t *client );

/*
 * Drain whatever is currently available on the data socket
 * (non-blocking) and invoke cb once per sample found. Safe to call
 * every frame regardless of whether the fd is actually readable --
 * it is a cheap no-op when there is nothing to read yet.
 *
 * Returns false on a fatal, unrecoverable socket error (sensorfwd
 * restarted, session lost, protocol desync) -- in that case stop
 * using this client and sensorfw_client_close() it.
 */
bool sensorfw_client_pump( sensorfw_client_t *client, sensorfw_sample_cb cb, void *user_data );

#ifdef __cplusplus
}
#endif

#endif /* SENSORFW_CLIENT_H */
