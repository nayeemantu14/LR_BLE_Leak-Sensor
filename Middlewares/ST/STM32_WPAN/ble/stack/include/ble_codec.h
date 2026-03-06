/*****************************************************************************
 * @file    ble_codec.h
 *
 * @brief   This file contains the interface of the BLE stack regarding
 *          isochronous data and audio codec.
 *****************************************************************************
 * @attention
 *
 * Copyright (c) 2018-2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 *****************************************************************************
 */

#ifndef BLE_CODEC_H__
#define BLE_CODEC_H__


#include <stdint.h>
#include "common_types.h"

/* Functions exported by the BLE stack
 */
uint8_t BLE_SendIsoDataToLinkLayer( uint16_t conn_handle,
                                    uint8_t pb_flag,
                                    uint8_t ts_flag,
                                    uint32_t timestamp,
                                    uint16_t PSN,
                                    uint16_t iso_data_load_length,
                                    uint16_t total_sdu_len,
                                    uint8_t* iso_data );

/* ISO command callbacks
 */
void BLECB_BigTerminateSync( uint8_t status,
                             uint8_t big_handle );

void BLECB_TerminateBig( uint8_t status,
                         uint8_t big_handle );

void BLECB_SetCigParameters( uint8_t status,
                             uint8_t CigId,
                             uint8_t CISCount,
                             uint16_t* conn_handle );

void BLECB_RemoveCig( uint8_t status,
                      uint8_t cig_id );

uint8_t BLECB_SetupIsoDataPath( uint16_t conn_handle,
                                uint8_t DataPathDirection,
                                uint8_t DataPathID,
                                const uint8_t CodecID[],
                                const uint8_t ControllerDelay[],
                                uint8_t CodecConfigurationLength,
                                const uint8_t* CodecConfiguration );

uint8_t BLECB_RemoveIsoDataPath( uint16_t conn_handle,
                                 uint8_t DataPathDirection );

/* ISO sync event callback
 */
void BLECB_SyncEvent( uint8_t group_id,
                      uint32_t next_anchor_point,
                      uint32_t time_stamp,
                      uint32_t next_sdu_delivery_timeout );

/* ISO data callback
 */
uint8_t BLECB_IsHciRxIsoDataPathOn( uint16_t conn_handle );

uint8_t BLECB_SendIsoData( uint16_t conn_handle,
                           uint8_t pb_flag,
                           uint8_t ts_flag,
                           uint32_t timestamp,
                           uint16_t PSN,
                           uint16_t iso_data_load_length,
                           uint16_t total_sdu_length,
                           uint8_t* iso_data );

/* Audio codec callbacks
 */
uint8_t BLECB_ConfigureDataPath( uint8_t data_path_direction,
                                 uint8_t data_pathID,
                                 uint8_t vendor_specific_config_length,
                                 const uint8_t* vendor_specific_config );

uint8_t BLECB_ReadLocalSupportedCodecs( uint8_t* num_supported_std_codecs,
                                      uint8_t* std_codec,
                                      uint8_t* num_Supported_vs_Codecs,
                                      uint8_t* vs_Codec );

uint8_t BLECB_ReadLocalSupportedCodecCapabilities(
                                         const uint8_t* codecID,
                                         uint8_t logical_transport_type,
                                         uint8_t direction,
                                         uint8_t* num_codec_capabilities,
                                         uint8_t* codec_capability );

uint8_t BLECB_ReadLocalSupportedControllerDelay(
                                         const uint8_t* codec_id,
                                         uint8_t logical_transport_type,
                                         uint8_t direction,
                                         uint8_t codec_configuration_length,
                                         const uint8_t* codec_configuration,
                                         uint8_t* min_controller_delay,
                                         uint8_t* max_controller_delay );

/* ISO calibration callback
 */
void BLECB_IsoCalibration( uint32_t timestamp );


#endif /* BLE_CODEC_H__ */
