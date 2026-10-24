#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "board_audio.h"
#include "rti_session.h"

/** Run an offline create/process/destroy smoke test for the S31 AEC library. */
esp_err_t webrtc_peer_aec_self_test(void);

/** Pre-generate and cache DTLS certificate material for repeatable reconnects. */
esp_err_t webrtc_peer_prepare(void);

/** Establish the RTI PeerConnection, exchange SDP, and open `oai-events`. */
esp_err_t webrtc_peer_start(const rti_session_t *session, const board_audio_t *audio);

/** The signaling callback allocates the returned answer; the peer frees it. */
typedef esp_err_t (*webrtc_group_exchange_cb_t)(const char *offer, char **answer, void *context);

typedef struct {
    uint64_t source_frames;
    uint64_t sent_packets;
    uint64_t received_packets;
    uint64_t decoded_samples;
    float source_rms_peak;
    float remote_rms_peak;
} webrtc_group_stats_t;

/** Establish one group audio Peer without opening an RTA session or DataChannel. */
esp_err_t webrtc_peer_start_group(const board_audio_t *audio,
                                  webrtc_group_exchange_cb_t exchange, void *context);

/** Explicit uplink gate. The controller must validate the matching floor grant first.
 *  Deadline uses esp_timer_get_time() units; false closes admission synchronously.
 *  Already admitted network/playout packets cannot be retracted.
 */
void webrtc_peer_group_set_sending(bool enabled, int64_t expires_at_us);
/** Close both group media admission gates synchronously, without I/O or wait.
 *  This Peer stays suspended until retired; a new group start resets the gate.
 *  Audio already admitted into network or I2S hardware cannot be retracted.
 */
void webrtc_peer_group_suspend_media(void);
bool webrtc_peer_group_connected(void);

/** Per-group-start counters, retained after stop. RMS is normalized to 0..1.
 *  decoded_samples counts PCM sample frames (one sample per channel).
 *  No audio recording is retained.
 */
void webrtc_peer_group_get_stats(webrtc_group_stats_t *stats);

/** Close the DataChannel and PeerConnection. Safe when no connection is active. */
void webrtc_peer_stop(void);
