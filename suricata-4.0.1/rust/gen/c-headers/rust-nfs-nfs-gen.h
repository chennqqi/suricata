/* Copyright (C) 2017 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/*
 * DO NOT EDIT. This file is automatically generated.
 */

#ifndef __RUST_NFS_NFS_GEN_H__
#define __RUST_NFS_NFS_GEN_H__

void * rs_nfs3_state_new(void);
void rs_nfs3_state_free(void * state);
int8_t rs_nfs3_parse_request(Flow * _flow, NFSState * state, void * _pstate, uint8_t * input, uint32_t input_len, void * _data);
int8_t rs_nfs3_parse_response(Flow * _flow, NFSState * state, void * _pstate, uint8_t * input, uint32_t input_len, void * _data);
int8_t rs_nfs3_parse_request_udp(Flow * _flow, NFSState * state, void * _pstate, uint8_t * input, uint32_t input_len, void * _data);
int8_t rs_nfs3_parse_response_udp(Flow * _flow, NFSState * state, void * _pstate, uint8_t * input, uint32_t input_len, void * _data);
uint64_t rs_nfs3_state_get_tx_count(NFSState * state);
NFSTransaction * rs_nfs3_state_get_tx(NFSState * state, uint64_t tx_id);
void rs_nfs3_state_tx_free(NFSState * state, uint64_t tx_id);
int rs_nfs3_state_progress_completion_status(uint8_t _direction);
uint8_t rs_nfs3_tx_get_alstate_progress(NFSTransaction * tx, uint8_t direction);
bool rs_nfs3_get_txs_updated(NFSState * state, uint8_t direction);
void rs_nfs3_reset_txs_updated(NFSState * state, uint8_t direction);
void rs_nfs3_tx_set_logged(NFSState * _state, NFSTransaction * tx, uint32_t logger);
int8_t rs_nfs3_tx_get_logged(NFSState * _state, NFSTransaction * tx, uint32_t logger);
uint8_t rs_nfs3_state_has_detect_state(NFSState * state);
void rs_nfs3_state_set_tx_detect_state(NFSState * state, NFSTransaction * tx, DetectEngineState * de_state);
DetectEngineState * rs_nfs3_state_get_tx_detect_state(NFSTransaction * tx);
uint8_t rs_nfs_state_has_events(NFSState * state);
AppLayerDecoderEvents * rs_nfs_state_get_events(NFSState * state, uint64_t tx_id);
int8_t rs_nfs_state_get_event_info(const char * event_name, int * event_id, AppLayerEventType * event_type);
uint8_t rs_nfs3_tx_get_procedures(NFSTransaction * tx, uint16_t i, uint32_t * procedure);
void rs_nfs_tx_get_version(NFSTransaction * tx, uint32_t * version);
void rs_nfs3_init(SuricataFileContext * context);
int8_t rs_nfs_probe_ts(const uint8_t * input, uint32_t len);
int8_t rs_nfs_probe_tc(const uint8_t * input, uint32_t len);
int8_t rs_nfs_probe_udp_ts(const uint8_t * input, uint32_t len);
int8_t rs_nfs_probe_udp_tc(const uint8_t * input, uint32_t len);
FileContainer * rs_nfs3_getfiles(uint8_t direction, NFSState * ptr);
void rs_nfs3_setfileflags(uint8_t direction, NFSState * ptr, uint16_t flags);

#endif /* ! __RUST_NFS_NFS_GEN_H__ */
