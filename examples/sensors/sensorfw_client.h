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
 * (see sensorfw_wire_xyz_t in sensorfw_client.c and docs/sensorfw.md)
 * -- re-verify with sensorfw_rawdump.c before trusting either on a
 * new build.
 *
 * Typical use:
 *   sensorfw_sensor_type_t have[SENSORFW_SENSOR_TYPE_COUNT];
 *   int n = sensorfw_client_list_available( have, SENSORFW_SENSOR_TYPE_COUNT );
 *   // pick have[i] == SENSORFW_SENSOR_GYROSCOPE if present, ...
 *   sensorfw_client_t *accel = sensorfw_client_open( SENSORFW_SENSOR_ACCELEROMETER );
 *   sensorfw_client_set_interval_ms( accel, 20 );  // ~50 Hz
 *   sensorfw_client_start( accel );
 *   // in the main loop, once per frame:
 *   sensorfw_client_pump( accel, on_sample, NULL );
 *   // on shutdown:
 *   sensorfw_client_close( accel );
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
 * Sensor types this client knows how to open and correctly parse
 * (its D-Bus interface name and its sensorfw_wire_xyz_t socket
 * layout are both confirmed on real hardware -- see
 * docs/sensorfw.md). sensorfw_client_open() only accepts one of
 * these, on purpose: a raw sensorfwd plugin id string could be
 * mistyped, or could name a sensor this client doesn't actually know
 * how to decode, and either mistake would only surface as garbage
 * data or a runtime D-Bus error instead of a compile-time one.
 *
 * To add a new type: probe it on-device with
 * sensorfw_client_open_raw() + sensorfw_rawdump.c first (see the
 * diagnostic section below), then add a row to SENSORFW_TYPES in
 * sensorfw_client.c and a matching entry here.
 */
typedef enum
{
	SENSORFW_SENSOR_ACCELEROMETER,
	SENSORFW_SENSOR_GYROSCOPE,
	SENSORFW_SENSOR_TYPE_COUNT /* not a real sensor -- array-sizing helper */
} sensorfw_sensor_type_t;

/* The underlying sensorfwd plugin id for this type (e.g.
 * "accelerometersensor") -- for logging/UI, and as the inverse of
 * sensorfw_sensor_type_from_name(). */
const char *sensorfw_sensor_type_name( sensorfw_sensor_type_t type );

/*
 * Parse a sensorfwd plugin id string (e.g. from
 * sensorfw_client_list_plugins_raw(), or a command-line argument)
 * back into a known type. Returns false if the string doesn't match
 * any entry in sensorfw_sensor_type_t -- which can simply mean this
 * client doesn't support that sensor yet, not that the string is
 * wrong.
 */
bool sensorfw_sensor_type_from_name( const char *name, sensorfw_sensor_type_t *out );

/*
 * Ask sensorfwd which sensors are actually present on this device
 * (D-Bus availableSensorPlugins()), intersected with the types this
 * client knows how to read. Fills `out` (caller-provided array of at
 * least SENSORFW_SENSOR_TYPE_COUNT entries) and returns how many
 * were written -- 0 if none, or if sensorfwd is unreachable. Opens
 * its own short-lived D-Bus connection; safe to call before any
 * sensorfw_client_open().
 */
int sensorfw_client_list_available( sensorfw_sensor_type_t *out, int max_types );

/*
 * One parsed sample. timestamp_us is sensorfwd's monotonic clock in
 * microseconds -- NOT wall-clock time and not synchronized to any
 * particular epoch; only useful for computing deltas between samples.
 *
 * x/y/z are passed through as float only for convenience -- sensorfwd
 * actually streams int32 values, not SI units or floating-point g.
 * The units are known and already calibrated: accelerometer in
 * milli-g (device flat -> z near 1000, x/y near 0), gyroscope in
 * milli-degrees per second. No calibration is required -- see
 * "Единицы и калибровка" in docs/sensorfw.md.
 */
typedef void (*sensorfw_sample_cb)( void *user_data, uint64_t timestamp_us,
                                     float x, float y, float z );

/*
 * Load the plugin, request a session and establish the data socket
 * for the given sensor type. Returns NULL on any failure (system bus
 * unreachable, sensorfwd not running, sensor not present on this
 * device, socket handshake failure) -- check stderr for the reason.
 * Use sensorfw_client_list_available() first if the sensor might not
 * exist on every target device.
 */
sensorfw_client_t *sensorfw_client_open( sensorfw_sensor_type_t type );

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

/* -------------------------------------------------------------------
 * Diagnostic-only API below -- NOT for reading sensor data in real
 * use, only for finding out whether a new sensor type is safe to add
 * to sensorfw_sensor_type_t above.
 * ---------------------------------------------------------------- */

/*
 * Full, unfiltered list of sensorfwd plugin ids on this device --
 * including ones sensorfw_sensor_type_t doesn't cover. Caller owns
 * the returned array; free with sensorfw_client_free_plugin_list().
 * Returns NULL (and *out_count = 0) if sensorfwd is unreachable.
 */
char **sensorfw_client_list_plugins_raw( int *out_count );
void sensorfw_client_free_plugin_list( char **list, int count );

/*
 * Open by a raw sensorfwd plugin id, bypassing sensorfw_sensor_type_t
 * entirely. If the id matches a known type its real D-Bus interface
 * is used; otherwise the interface name is *guessed* from the id by
 * the local.<Type>Sensor convention seen for the known types --
 * acronym-style ids (e.g. "alssensor") likely need different
 * capitalization, so treat the guess as a starting point to confirm
 * with dbus-send introspection, not a fact.
 *
 * sensorfw_client_pump() on the result still assumes the
 * sensorfw_wire_xyz_t layout, which is unverified for anything other
 * than the known types -- read raw bytes with sensorfw_rawdump.c
 * instead of trusting pump() here.
 */
sensorfw_client_t *sensorfw_client_open_raw( const char *sensor_id );

#ifdef __cplusplus
}
#endif

#endif /* SENSORFW_CLIENT_H */
