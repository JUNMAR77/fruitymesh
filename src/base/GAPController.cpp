/**

Copyright (c) 2014-2015 "M-Way Solutions GmbH"
FruityMesh - Bluetooth Low Energy mesh protocol [http://mwaysolutions.com/]

This file is part of FruityMesh

FruityMesh is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <Config.h>
#include <Node.h>
#include <AdvertisingController.h>
#include <ScanController.h>
#include <Logger.h>
#include <GAPController.h>

#include <Utility.h>

extern "C"{
#include <app_error.h>
#include <ble.h>
#include <ble_gap.h>
#include <ble_hci.h>
#include <ble_radio_notification.h>
}




//Initialize
void (*GAPController::connectionSuccessCallback)(ble_evt_t* bleEvent);
void (*GAPController::connectionEncryptedCallback)(ble_evt_t* bleEvent);
void (*GAPController::connectionTimeoutCallback)(ble_evt_t* bleEvent);
void (*GAPController::disconnectionCallback)(ble_evt_t* bleEvent);

bool GAPController::currentlyConnecting = false;

//This function configures the Generic access Profile
//GAP is a service that must be implemented on all BLE devices
//These are some basic values that other devices can request
//Some of these values might be writable over the air
//The current characteristics are
//		Device name
//		Appearance
//		Peripheral Preferred Connection Parameters
void GAPController::bleConfigureGAP(){
	u32 err = 0;
	//Set up an open link write permission
	//There are multiple security modes defined in the BLE spec
	//Use these macros: http://developer.nordicsemi.com/nRF51_SDK/doc/7.1.0/s110/html/a00813.html
	ble_gap_conn_sec_mode_t secPermissionOpen;
	BLE_GAP_CONN_SEC_MODE_SET_OPEN(&secPermissionOpen);

	//Set the GAP device name
	err = sd_ble_gap_device_name_set(&secPermissionOpen, (u8*)DEVICE_NAME, strlen(DEVICE_NAME));
	APP_ERROR_CHECK(err);

	//Set the appearance of the device as defined in http://developer.nordicsemi.com/nRF51_SDK/doc/7.1.0/s110/html/a00837.html
	err = sd_ble_gap_appearance_set(BLE_APPEARANCE_GENERIC_COMPUTER);
	APP_ERROR_CHECK(err);

	//Set gap peripheral preferred connection parameters (not used by the mesh implementation)
	ble_gap_conn_params_t gapConnectionParams;
	memset(&gapConnectionParams, 0, sizeof(gapConnectionParams));
	gapConnectionParams.min_conn_interval = Config->meshMinConnectionInterval;
	gapConnectionParams.max_conn_interval = Config->meshMaxConnectionInterval;
	gapConnectionParams.slave_latency = Config->meshPeripheralSlaveLatency;
	gapConnectionParams.conn_sup_timeout = Config->meshConnectionSupervisionTimeout;
	err = sd_ble_gap_ppcp_set(&gapConnectionParams);
	APP_ERROR_CHECK(err);


}


//Connect to a specific peripheral
bool GAPController::connectToPeripheral(ble_gap_addr_t* address)
{
	if(currentlyConnecting) return false;

	currentlyConnecting = true;

	u32 err = 0;

	ble_gap_scan_params_t scan_params;
	ble_gap_conn_params_t conn_params;

	scan_params.selective = 0;
	scan_params.active = 0; /* No active scanning */
	scan_params.interval = Config->meshConnectingScanInterval;
	scan_params.window = Config->meshConnectingScanWindow;
	scan_params.timeout = Config->meshConnectingScanTimeout;

	conn_params.min_conn_interval = Config->meshMinConnectionInterval;
	conn_params.max_conn_interval = Config->meshMaxConnectionInterval;
	conn_params.slave_latency = Config->meshPeripheralSlaveLatency;
	conn_params.conn_sup_timeout = Config->meshConnectionSupervisionTimeout;

	//Connect to the peripheral
	err = sd_ble_gap_connect(address, &scan_params, &conn_params);
	APP_ERROR_CHECK(err);

	//Set scan state off. Connecting uses scan itself and deactivates the scan procedure
	ScanController::scanningState = SCAN_STATE_OFF;

	logt("CONN", "pairing");

	return true;
}


//Disconnect from paired peripheral
void GAPController::disconnectFromPeripheral(u16 connectionHandle)
{
	u32 err = 0;

	err = sd_ble_gap_disconnect(connectionHandle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
	APP_ERROR_CHECK(err);
}





bool GAPController::bleConnectionEventHandler(ble_evt_t* bleEvent)
{
	u32 err = 0;

	//Depending on the type of the BLE event, we have to do different stuff
	//Further Events: http://developer.nordicsemi.com/nRF51_SDK/doc/7.1.0/s120/html/a00746.html#gada486dd3c0cce897b23a887bed284fef
	switch (bleEvent->header.evt_id)
	{
	//************************** GAP *****************************
	//########## Connection with other device
	case BLE_GAP_EVT_CONNECTED:
	{
		currentlyConnecting = false;

		// Our advertisement stopped because we received a connection on this packet (we are peripheral)
		if (bleEvent->evt.gap_evt.params.connected.role == BLE_GAP_ROLE_PERIPH){
			AdvertisingController::AdvertisingInterruptedBecauseOfIncomingConnectionHandler();
		}

		logt("C", "Connected device");

		//Connection stops advertising
		connectionSuccessCallback(bleEvent);

		return true;
	}
		//########## Another Device disconnected
	case BLE_GAP_EVT_DISCONNECTED:
	{
		logt("C", "Disconnected device %d", bleEvent->evt.gap_evt.params.disconnected.reason);
		disconnectionCallback(bleEvent);

		return true;
	}
		break;

	case BLE_GAP_EVT_TIMEOUT:
	{
		if (bleEvent->evt.gap_evt.params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN)
		{
			currentlyConnecting = false;

			connectionTimeoutCallback(bleEvent);
		}
		break;
	}
	//The other device requested an encrypted connection
	case BLE_GAP_EVT_SEC_INFO_REQUEST:
	{
		//With this request, we receive the key identifier and a random number
		//This identification is used to select the correct key
		//We skip that process and select our mesh network key
		//TODO: Check what to do when we want different keys for Modules e.g.
		ble_gap_evt_sec_info_request_t securityRequest = bleEvent->evt.gap_evt.params.sec_info_request;

		//This is our security key
		ble_gap_enc_info_t key;
		key.auth = 1; //This key is authenticated
		memcpy(&key.ltk, Node::getInstance()->persistentConfig.networkKey, 16); //Copy our mesh network key
		key.ltk_len = 16;

		//Reply  with our stored key
		err = sd_ble_gap_sec_info_reply(
			bleEvent->evt.gap_evt.conn_handle,
			&key, //This is our stored long term key
			NULL, //We do not have an identity resolving key
			NULL //We do not have signing info
		);
		APP_ERROR_CHECK(err);

		logt("SEC", "SEC_INFO_REQUEST received, replying with key. result %d",  err);

		break;
	}
	//This event tells us that the security keys are now in use
	case BLE_GAP_EVT_CONN_SEC_UPDATE:
	{
		u8 keySize = bleEvent->evt.gap_evt.params.conn_sec_update.conn_sec.encr_key_size;

		u8 level = bleEvent->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.lv;
		u8 securityMode = bleEvent->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.sm;

		logt("SEC", "Connection key is now %u bytes, level %u, securityMode %u", keySize, level, securityMode);

		if(connectionEncryptedCallback) connectionEncryptedCallback(bleEvent);



		break;

	}
	//Another device wants to initiate a secure connection
	//Currently not used because the other method worked pretty good
	/*case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
	{

		//Security parameters: What is provided and what capabilities do we have?
		ble_gap_sec_params_t securityParameters;
		securityParameters.bond = 0; //We do not want to bond
		securityParameters.mitm = 1; //We need man in the middle protection
		securityParameters.io_caps = BLE_GAP_IO_CAPS_KEYBOARD_ONLY; //Use this if we want OOB method
		securityParameters.oob = 1; //We have out of band data (our mesh network key)
		securityParameters.min_key_size = 16; //We want 128bit key
		securityParameters.max_key_size = 16; //yes, we do
		securityParameters.kdist_periph.enc = 1; //We will provide a long term key for the connection
		securityParameters.kdist_periph.id = 0; //We do not provide an identity resolving key
		securityParameters.kdist_periph.sign = 0; //We do not provide a signature resolving key
		//Mhhh, ....?
		securityParameters.kdist_central.enc = 1;
		securityParameters.kdist_central.id = 0;
		securityParameters.kdist_central.sign = 0;


		ble_gap_enc_key_t key;
		key.enc_info.auth = 1;
		memcpy(&key.enc_info.ltk, &Config->meshNetworkKey, 16);
		key.enc_info.ltk_len = 16;

		key.master_id.ediv = 123;
		key.master_id.rand[0] = 234;



		//Keys
		ble_gap_sec_keyset_t keys;

		keys.keys_periph.p_enc_key = &key;
		keys.keys_periph.p_id_key = NULL;
		keys.keys_periph.p_sign_key = NULL;

		keys.keys_central.p_enc_key = &key;
		keys.keys_central.p_id_key = NULL;
		keys.keys_central.p_sign_key = NULL;


		//If we are central
		if(TestModule::isCentral)
		{
			err = sd_ble_gap_sec_params_reply(
					bleEvent->evt.gap_evt.conn_handle,
					BLE_GAP_SEC_STATUS_SUCCESS,
					NULL,
					&keys
				);

			logt("SEC", "SEC_PARAMS_REQUEST received as central, replying with result %d", err);
		}


		//We assume that we are peripheral
		else if(!TestModule::isCentral)
		{ //TODO: Check if we are peripheral




			err = sd_ble_gap_sec_params_reply(
					bleEvent->evt.gap_evt.conn_handle,
					BLE_GAP_SEC_STATUS_SUCCESS,
					&securityParameters,
					&keys
			);
			APP_ERROR_CHECK(err);


		}

		logt("SEC", "SEC_PARAMS_REQUEST received as peripheral, replying with result %d", err);


		break;
	}
	case BLE_GAP_EVT_AUTH_KEY_REQUEST:
	{
		err = sd_ble_gap_auth_key_reply(
			bleEvent->evt.gap_evt.conn_handle,
			BLE_GAP_AUTH_KEY_TYPE_OOB,
			Config->meshNetworkKey
		);
		APP_ERROR_CHECK(err);

		logt("SEC", "AUTH_KEY_REQUEST received, replying with result %d", err);

		break;
	}
	case BLE_GAP_EVT_AUTH_STATUS:
	{
		logt("SEC", "AUTH_STATUS: status %u", bleEvent->evt.gap_evt.params.auth_status.auth_status);
		logt("SEC", "AUTH_STATUS: bonded %u", bleEvent->evt.gap_evt.params.auth_status.bonded);

			break;
	}*/


	default:
		break;
	}
	return false;
}

void GAPController::setConnectionTimeoutHandler(void (*callback)(ble_evt_t* bleEvent))
{
	connectionTimeoutCallback = callback;
}

void GAPController::setConnectionSuccessfulHandler(void (*callback)(ble_evt_t* bleEvent))
{
	connectionSuccessCallback = callback;
}

void GAPController::setConnectionEncryptedHandler(void (*callback)(ble_evt_t* bleEvent))
{
	connectionEncryptedCallback = callback;
}

void GAPController::setDisconnectionHandler(void (*callback)(ble_evt_t* bleEvent))
{
	disconnectionCallback = callback;
}

void GAPController::startEncryptingConnection(u16 connectionHandle)
{
	u32 err = 0;

	//Key identification data
	//We do not need a key identification currently, because we only have one
	ble_gap_master_id_t keyId;
	keyId.ediv = 0;
	memset(&keyId.rand, 0, 16);

	//Our mesh network key
	ble_gap_enc_info_t key;
	key.auth = 1;
	memcpy(&key.ltk, Node::getInstance()->persistentConfig.networkKey, 16);
	key.ltk_len = 16;


	//This starts the Central Encryption Establishment using stored keys
	//http://infocenter.nordicsemi.com/topic/com.nordic.infocenter.s130.api.v1.0.0/group___b_l_e___g_a_p___c_e_n_t_r_a_l___e_n_c___m_s_c.html
	//http://infocenter.nordicsemi.com/topic/com.nordic.infocenter.s130.api.v1.0.0/group___b_l_e___g_a_p___p_e_r_i_p_h___e_n_c___m_s_c.html
	err = sd_ble_gap_encrypt(connectionHandle, &keyId, &key);
	APP_ERROR_CHECK(err);

	logt("SEC", "encrypting connection handle %u with key. result %u", connectionHandle, err);

	/*//Method 2: Central bonding: Passkey entry or OOB MSC
		//Security parameters: What is provided and what capabilities do we have?
		ble_gap_sec_params_t securityParameters;
		securityParameters.bond = 0; //We do not want to bond
		securityParameters.mitm = 1; //We need man in the middle protection
		securityParameters.io_caps = BLE_GAP_IO_CAPS_KEYBOARD_ONLY; //Use this if we want OOB method
		securityParameters.oob = 1; //We have out of band data (our mesh network key)
		securityParameters.min_key_size = 16; //We want 128bit key
		securityParameters.max_key_size = 16; //yes, we do
		securityParameters.kdist_periph.enc = 1; //We will provide a long term key for the connection
		securityParameters.kdist_periph.id = 0; //We do not provide an identity resolving key
		securityParameters.kdist_periph.sign = 0; //We do not provide a signature resolving key
		//Mhhh, ....?
		securityParameters.kdist_central.enc = 1;
		securityParameters.kdist_central.id = 0;
		securityParameters.kdist_central.sign = 0;

		err = sd_ble_gap_authenticate(connectionHandle, &securityParameters);

		logt("SEC", "encrypting connection %u with result %u", connectionHandle, err);
	*/
}
