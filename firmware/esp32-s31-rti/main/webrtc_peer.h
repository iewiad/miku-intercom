#pragma once

#include "esp_err.h"
#include "board_audio.h"
#include "rti_session.h"

/** Run an offline create/process/destroy smoke test for the S31 AEC library. */
esp_err_t webrtc_peer_aec_self_test(void);

/** Pre-generate and cache DTLS certificate material for repeatable reconnects. */
esp_err_t webrtc_peer_prepare(void);

/** Establish the RTI PeerConnection, exchange SDP, and open `oai-events`. */
esp_err_t webrtc_peer_start(const rti_session_t *session, const board_audio_t *audio);

/** Close the DataChannel and PeerConnection. Safe when no connection is active. */
void webrtc_peer_stop(void);
