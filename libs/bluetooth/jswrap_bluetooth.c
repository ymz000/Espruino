/* Copyright (c) 2014 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

/** @file
 *
 * @defgroup ble_sdk_uart_over_ble_main main.c
 * @{
 * @ingroup  ble_sdk_app_nus_eval
 * @brief    UART over BLE application main file.
 *
 * This file contains the source code for a sample application that uses the Nordic UART service.
 * This application uses the @ref srvlib_conn_params module.
 */

#include "jswrap_bluetooth.h"
#include "jsinteractive.h"
#include "jsdevices.h"

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "nordic_common.h"
#include "nrf.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "softdevice_handler.h"
#include "app_timer.h"
#include "ble_nus.h"
#include "app_util_platform.h"

#include "device_manager.h"
#include "pstorage.h"
#include "ble_dfu.h"
#include "dfu_app_handler.h"
#include "nrf_delay.h"

#define IS_SRVC_CHANGED_CHARACT_PRESENT 1                                           /**< Include the service_changed characteristic. If not enabled, the server's database cannot be changed for the lifetime of the device. */

#ifdef NRF52
// nRF52 gets the ability to connect to other
#define CENTRAL_LINK_COUNT              1                                           /**<number of central links used by the application. When changing this number remember to adjust the RAM settings*/
#define PERIPHERAL_LINK_COUNT           1                                           /**<number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/
#else
#define CENTRAL_LINK_COUNT              0                                           /**<number of central links used by the application. When changing this number remember to adjust the RAM settings*/
#define PERIPHERAL_LINK_COUNT           1                                           /**<number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/
#endif

// Working out the amount of RAM we need - see softdevice_handler.h
#define IDEAL_RAM_START_ADDRESS_INTERN(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT) \
  APP_RAM_BASE_CENTRAL_LINKS_##CENTRAL_LINK_COUNT##_PERIPH_LINKS_##PERIPHERAL_LINK_COUNT##_SEC_COUNT_0_MID_BW
#define IDEAL_RAM_START_ADDRESS(C_LINK_CNT, P_LINK_CNT) IDEAL_RAM_START_ADDRESS_INTERN(C_LINK_CNT, P_LINK_CNT)

#define DEVICE_NAME                     "Espruino "PC_BOARD_ID                      /**< Name of device. Will be included in the advertising data. */
#define NUS_SERVICE_UUID_TYPE           BLE_UUID_TYPE_VENDOR_BEGIN                  /**< UUID type for the Nordic UART Service (vendor specific). */

#define APP_ADV_INTERVAL                600                                        /**< The advertising interval (in units of 0.625 ms. This value corresponds to 750ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      180                                         /**< The advertising timeout (in units of seconds). */

#define SCAN_INTERVAL                   0x00A0                                      /**< Scan interval in units of 0.625 millisecond. 100ms */
#define SCAN_WINDOW                     0x00A0                                      /**< Scan window in units of 0.625 millisecond. 100ms */
// We want to listen as much of the time as possible. Not sure if 100/100 is feasible (50/100 is what's used in examples),
// but it seems to work fine like this.

#define APP_TIMER_PRESCALER             0                                           /**< Value of the RTC1 PRESCALER register. */
#define APP_TIMER_OP_QUEUE_SIZE         1                                           /**< Size of timer operation queues. */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(20, UNIT_1_25_MS)             /**< Minimum acceptable connection interval (20 ms), Connection interval uses 1.25 ms units. */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(75, UNIT_1_25_MS)             /**< Maximum acceptable connection interval (75 ms), Connection interval uses 1.25 ms units. */
#define SLAVE_LATENCY                   0                                           /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)             /**< Connection supervisory timeout (4 seconds), Supervision Timeout uses 10 ms units. */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000, APP_TIMER_PRESCALER)  /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000, APP_TIMER_PRESCALER) /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */

#define SEC_PARAM_BOND                   1                                          /**< Perform bonding. */
#define SEC_PARAM_MITM                   0                                          /**< Man In The Middle protection not required. */
#define SEC_PARAM_IO_CAPABILITIES        BLE_GAP_IO_CAPS_NONE                       /**< No I/O capabilities. */
#define SEC_PARAM_OOB                    0                                          /**< Out Of Band data not available. */
#define SEC_PARAM_MIN_KEY_SIZE           7                                          /**< Minimum encryption key size. */
#define SEC_PARAM_MAX_KEY_SIZE           16                                         /**< Maximum encryption key size. */

#define DFU_REV_MAJOR                    0x00                                       /** DFU Major revision number to be exposed. */
#define DFU_REV_MINOR                    0x01                                       /** DFU Minor revision number to be exposed. */
#define DFU_REVISION                     ((DFU_REV_MAJOR << 8) | DFU_REV_MINOR)     /** DFU Revision number to be exposed. Combined of major and minor versions. */
#define APP_SERVICE_HANDLE_START         0x000C                                     /**< Handle of first application specific service when when service changed characteristic is present. */
#define BLE_HANDLE_MAX                   0xFFFF                                     /**< Max handle value in BLE. */

#define DEAD_BEEF                       0xDEADBEEF                                  /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

static ble_nus_t                        m_nus;                                      /**< Structure to identify the Nordic UART Service. */
static uint16_t                         m_conn_handle = BLE_CONN_HANDLE_INVALID;    /**< Handle of the current connection. */
#if CENTRAL_LINK_COUNT>0
static uint16_t                         m_central_conn_handle = BLE_CONN_HANDLE_INVALID; /**< Handle for central mode connection */

#include "ble_db_discovery.h"
static ble_db_discovery_t               m_ble_db_discovery;             /**< Instance of database discovery module. Must be passed to all db_discovert API calls */
static void db_disc_handler(ble_db_discovery_evt_t * p_evt);
#endif

static ble_uuid_t                       m_adv_uuids[] = {{BLE_UUID_NUS_SERVICE, NUS_SERVICE_UUID_TYPE}};  /**< Universally unique service identifier. */

static ble_dfu_t                        m_dfus;                                    /**< Structure used to identify the DFU service. */
static dm_application_instance_t        m_app_handle;                              /**< Application identifier allocated by device manager */

typedef enum  {
  BLE_NONE = 0,
  BLE_IS_SENDING = 1,
  BLE_IS_SCANNING = 2,
} BLEStatus;

static volatile BLEStatus bleStatus;

#define BLE_SCAN_EVENT                  JS_EVENT_PREFIX"blescan"
#define BLE_WRITE_EVENT                 JS_EVENT_PREFIX"blew"

/**@brief Error handlers.
 *
 * @details
 */
void ble_app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name) {
  jsiConsolePrintf("NRF ERROR 0x%x at %s:%d\n", error_code, p_file_name, line_num);
  jsiConsolePrint("REBOOTING.\n");
  jshTransmitFlush();
  NVIC_SystemReset();
}

/**@brief Function for assert macro callback.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyse 
 *          how your product is supposed to react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in] line_num    Line number of the failing ASSERT call.
 * @param[in] p_file_name File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    ble_app_error_handler(DEAD_BEEF, line_num, p_file_name);
}


/**@brief Function for stopping advertising.
 */
static void advertising_stop(void)
{
    uint32_t err_code;

    err_code = sd_ble_gap_adv_stop();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for loading application-specific context after establishing a secure connection.
 *
 * @details This function will load the application context and check if the ATT table is marked as
 *          changed. If the ATT table is marked as changed, a Service Changed Indication
 *          is sent to the peer if the Service Changed CCCD is set to indicate.
 *
 * @param[in] p_handle The Device Manager handle that identifies the connection for which the context
 *                     should be loaded.
 */
/*static void app_context_load(dm_handle_t const * p_handle)
{
    uint32_t                 err_code;
    static uint32_t          context_data;
    dm_application_context_t context;

    context.len    = sizeof(context_data);
    context.p_data = (uint8_t *)&context_data;

    err_code = dm_application_context_get(p_handle, &context);
    if (err_code == NRF_SUCCESS)
    {
        // Send Service Changed Indication if ATT table has changed.
        if ((context_data & (DFU_APP_ATT_TABLE_CHANGED << DFU_APP_ATT_TABLE_POS)) != 0)
        {
            err_code = sd_ble_gatts_service_changed(m_conn_handle, APP_SERVICE_HANDLE_START, BLE_HANDLE_MAX);
            if ((err_code != NRF_SUCCESS) &&
                (err_code != BLE_ERROR_INVALID_CONN_HANDLE) &&
                (err_code != NRF_ERROR_INVALID_STATE) &&
                (err_code != BLE_ERROR_NO_TX_PACKETS) &&
                (err_code != NRF_ERROR_BUSY) &&
                (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING))
            {
                APP_ERROR_HANDLER(err_code);
            }
        }

        err_code = dm_application_context_delete(p_handle);
        APP_ERROR_CHECK(err_code);
    }
    else if (err_code == DM_NO_APP_CONTEXT)
    {
        // No context available. Ignore.
    }
    else
    {
        APP_ERROR_HANDLER(err_code);
    }
}*/


/**@brief Function for preparing for system reset.
 *
 * @details This function implements @ref dfu_app_reset_prepare_t. It will be called by
 *          @ref dfu_app_handler.c before entering the bootloader/DFU.
 *          This allows the current running application to shut down gracefully.
 */
static void reset_prepare(void)
{
    uint32_t err_code;
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
    {
        // Disconnect from peer.
        err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        APP_ERROR_CHECK(err_code);
    }
    else
    {
        // If not connected, the device will be advertising. Hence stop the advertising.
        advertising_stop();
    }
    err_code = ble_conn_params_stop();
    APP_ERROR_CHECK(err_code);
    nrf_delay_ms(500);

    jsiKill();
    jsvKill();
    jshKill();
    jshReset();
    nrf_delay_ms(100);
}


/**@brief Function for the GAP initialization.
 *
 * @details This function will set up all the necessary GAP (Generic Access Profile) parameters of 
 *          the device. It also sets the permissions and appearance.
 */
static void gap_params_init(void)
{
    uint32_t                err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);
    
    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *) DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling the data from the Nordic UART Service.
 *
 * @details This function will process the data received from the Nordic UART BLE Service and send
 *          it to the terminal.
 *
 * @param[in] p_nus    Nordic UART Service structure.
 * @param[in] p_data   Data to be send to UART module.
 * @param[in] length   Length of the data.
 */
/**@snippet [Handling the data received over BLE] */
static void nus_data_handler(ble_nus_t * p_nus, uint8_t * p_data, uint16_t length)
{
    uint32_t i;
    for (i = 0; i < length; i++) {
      jshPushIOCharEvent(EV_BLUETOOTH, (char) p_data[i]);
    }
}

bool jswrap_nrf_transmit_string() {
  if (m_conn_handle == BLE_CONN_HANDLE_INVALID) {
    // If no connection, drain the output buffer
    while (jshGetCharToTransmit(EV_BLUETOOTH)>=0);
  }
  if (bleStatus & BLE_IS_SENDING) return false;
  static uint8_t buf[BLE_NUS_MAX_DATA_LEN];
  int idx = 0;
  int ch = jshGetCharToTransmit(EV_BLUETOOTH);
  while (ch>=0) {
    buf[idx++] = ch;
    if (idx>=BLE_NUS_MAX_DATA_LEN) break;
    ch = jshGetCharToTransmit(EV_BLUETOOTH);
  }
  if (idx>0) {
    if (ble_nus_string_send(&m_nus, buf, idx) == NRF_SUCCESS)
      bleStatus |= BLE_IS_SENDING;
  }
  return idx>0;
}

/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
    uint32_t       err_code;
    ble_nus_init_t nus_init;
    
    memset(&nus_init, 0, sizeof(nus_init));

    nus_init.data_handler = nus_data_handler;
    
    err_code = ble_nus_init(&m_nus, &nus_init);
    APP_ERROR_CHECK(err_code);

    ble_dfu_init_t   dfus_init;

    // Initialize the Device Firmware Update Service.
    memset(&dfus_init, 0, sizeof(dfus_init));

    dfus_init.evt_handler   = dfu_app_on_dfu_evt;
    dfus_init.error_handler = NULL;
    dfus_init.evt_handler   = dfu_app_on_dfu_evt;
    dfus_init.revision      = DFU_REVISION;

    err_code = ble_dfu_init(&m_dfus, &dfus_init);
    APP_ERROR_CHECK(err_code);

    dfu_app_reset_prepare_set(reset_prepare);
    dfu_app_dm_appl_instance_set(m_app_handle);
}


/**@brief Function for handling an event from the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module
 *          which are passed to the application.
 *
 * @note All this function does is to disconnect. This could have been done by simply setting
 *       the disconnect_on_fail config parameter, but instead we use the event handler
 *       mechanism to demonstrate its use.
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 */
static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    uint32_t err_code;
    
    if(p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
    {
        err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}


/**@brief Function for handling errors from the Connection Parameters module.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void)
{
    uint32_t               err_code;
    ble_conn_params_init_t cp_init;
    
    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;
    
    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}




void jswrap_nrf_bluetooth_startAdvertise(void) {
  uint32_t err_code = 0;
  // Actually start advertising
  ble_gap_adv_params_t adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.type        = BLE_GAP_ADV_TYPE_ADV_IND;
  adv_params.p_peer_addr = NULL;
  adv_params.fp          = BLE_GAP_ADV_FP_ANY;
  adv_params.p_whitelist = NULL;
  adv_params.timeout  = APP_ADV_TIMEOUT_IN_SECONDS;
  adv_params.interval = APP_ADV_INTERVAL;

  err_code = sd_ble_gap_adv_start(&adv_params);
  APP_ERROR_CHECK(err_code);
}

/// Get the correct event name for a BLE write event to a characteristic (eventName should be 12 chars long)
void ble_handle_to_write_event_name(char *eventName, uint16_t handle) {
  strcpy(eventName, BLE_WRITE_EVENT);
  itostr(handle, &eventName[strlen(eventName)], 16);
}

/**@brief Function for the application's SoftDevice event handler.
 *
 * @param[in] p_ble_evt SoftDevice event.
 */
static void on_ble_evt(ble_evt_t * p_ble_evt)
{
    uint32_t                         err_code;
    jsiConsolePrintf("\n[%d]\n", p_ble_evt->header.evt_id);
    
    switch (p_ble_evt->header.evt_id) {
      case BLE_GAP_EVT_TIMEOUT:
        // the timeout for sd_ble_gap_adv_start expired - kick it off again
        jswrap_nrf_bluetooth_startAdvertise();
        break;

      case BLE_GAP_EVT_CONNECTED:
        if (p_ble_evt->evt.gap_evt.params.connected.role == BLE_GAP_ROLE_PERIPH) {
          m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
          bleStatus &= ~BLE_IS_SENDING; // reset state - just in case
          jsiSetConsoleDevice( EV_BLUETOOTH );
        }
#if CENTRAL_LINK_COUNT>0
        if (p_ble_evt->evt.gap_evt.params.connected.role == BLE_GAP_ROLE_CENTRAL) {
          m_central_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
          // TODO: fire connect event on 'NRF'
        }
#endif
        break;

      case BLE_GAP_EVT_DISCONNECTED:
#if CENTRAL_LINK_COUNT>0
        if (m_central_conn_handle == p_ble_evt->evt.gap_evt.conn_handle) {
          m_central_conn_handle = BLE_CONN_HANDLE_INVALID;
          // TODO: fire disconnect event on 'NRF'
          break;
        }
#endif
        m_conn_handle = BLE_CONN_HANDLE_INVALID;
        jsiSetConsoleDevice( DEFAULT_CONSOLE_DEVICE );
        // restart advertising after disconnection
        jswrap_nrf_bluetooth_startAdvertise();
        break;

      case BLE_GATTS_EVT_SYS_ATTR_MISSING:
        // No system attributes have been stored.
        err_code = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
        APP_ERROR_CHECK(err_code);
        break;

      case BLE_EVT_TX_COMPLETE:
        // UART Transmit finished - we can try and send more data
        bleStatus &= ~BLE_IS_SENDING;
        jswrap_nrf_transmit_string();
        break;

      case BLE_GAP_EVT_ADV_REPORT: {
        // Advertising data received
        const ble_gap_evt_adv_report_t *p_adv = &p_ble_evt->evt.gap_evt.params.adv_report;

        JsVar *evt = jsvNewObject();
        if (evt) {
          jsvObjectSetChildAndUnLock(evt, "rssi", jsvNewFromInteger(p_adv->rssi));
          //jsvObjectSetChildAndUnLock(evt, "addr_type", jsvNewFromInteger(p_adv->peer_addr.addr_type));
          jsvObjectSetChildAndUnLock(evt, "addr", jsvVarPrintf("%02x:%02x:%02x:%02x:%02x:%02x",
              p_adv->peer_addr.addr[5],
              p_adv->peer_addr.addr[4],
              p_adv->peer_addr.addr[3],
              p_adv->peer_addr.addr[2],
              p_adv->peer_addr.addr[1],
              p_adv->peer_addr.addr[0]));
          JsVar *data = jsvNewStringOfLength(p_adv->dlen);
          if (data) {
            jsvSetString(data, (char*)p_adv->data, p_adv->dlen);
            JsVar *ab = jsvNewArrayBufferFromString(data, p_adv->dlen);
            jsvUnLock(data);
            jsvObjectSetChildAndUnLock(evt, "data", ab);
          }
          jsiQueueObjectCallbacks(execInfo.root, BLE_SCAN_EVENT, &evt, 1);
          jsvUnLock(evt);
        }
        break;
        }

      case BLE_GATTS_EVT_WRITE: {
        ble_gatts_evt_write_t * p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;
        // We got a param write event - add this to the object callback queue
        JsVar *evt = jsvNewObject();
        if (evt) {
          JsVar *data = jsvNewStringOfLength(p_evt_write->len);
          if (data) {
            jsvSetString(data, (char*)p_evt_write->data, p_evt_write->len);
            JsVar *ab = jsvNewArrayBufferFromString(data, p_evt_write->len);
            jsvUnLock(data);
            jsvObjectSetChildAndUnLock(evt, "data", ab);
          }
          char eventName[12];
          ble_handle_to_write_event_name(eventName, p_evt_write->handle);
          jsiQueueObjectCallbacks(execInfo.root, eventName, &evt, 1);
          jsvUnLock(evt);
        }
        break;
      }
      default:
          // No implementation needed.
          break;
    }
}


/**@brief Function for dispatching a SoftDevice event to all modules with a SoftDevice 
 *        event handler.
 *
 * @details This function is called from the SoftDevice event interrupt handler after a 
 *          SoftDevice event has been received.
 *
 * @param[in] p_ble_evt  SoftDevice event.
 */
static void ble_evt_dispatch(ble_evt_t * p_ble_evt)
{
    ble_conn_params_on_ble_evt(p_ble_evt);
    ble_nus_on_ble_evt(&m_nus, p_ble_evt);
    ble_dfu_on_ble_evt(&m_dfus, p_ble_evt);
    ble_db_discovery_on_ble_evt(&m_ble_db_discovery, p_ble_evt);
    on_ble_evt(p_ble_evt);

    dm_ble_evt_handler(p_ble_evt); //Add this line
}


/**@brief Function for dispatching a system event to interested modules.
 *
 * @details This function is called from the System event interrupt handler after a system
 *          event has been received.
 *
 * @param[in] sys_evt  System stack event.
 */
static void sys_evt_dispatch(uint32_t sys_evt)
{
    pstorage_sys_event_handler(sys_evt);
    ble_advertising_on_sys_evt(sys_evt);
}


/**@brief Function for the SoftDevice initialization.
 *
 * @details This function initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    uint32_t err_code;
    
    // Initialize SoftDevice.
    // SOFTDEVICE_HANDLER_INIT(NRF_CLOCK_LFCLKSRC_XTAL_20_PPM, NULL); // Maybe we should use this if external crystal available.
    SOFTDEVICE_HANDLER_INIT(NRF_CLOCK_LFCLKSRC_RC_250_PPM_TEMP_8000MS_CALIBRATION, false);
    
    ble_enable_params_t ble_enable_params;
    err_code = softdevice_enable_get_default_config(CENTRAL_LINK_COUNT,
                                                    PERIPHERAL_LINK_COUNT,
                                                    &ble_enable_params);
    APP_ERROR_CHECK(err_code);
    
    ble_enable_params.common_enable_params.vs_uuid_count  = 2;
    ble_enable_params.gatts_enable_params.service_changed = 1;

    //Check the ram settings against the used number of links
    CHECK_RAM_START_ADDR(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT);

    extern void __data_start__;
    if (IDEAL_RAM_START_ADDRESS(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT) != (uint32_t)&__data_start__) {
      jsiConsolePrintf("WARNING: BLE RAM start address not correct - is 0x%x, should be 0x%x\n\n", (uint32_t)&__data_start__, IDEAL_RAM_START_ADDRESS(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT));
      jshTransmitFlush();
    }

    // Enable BLE stack.
    err_code = softdevice_enable(&ble_enable_params);
    APP_ERROR_CHECK(err_code);
    
    // Subscribe for BLE events.
    err_code = softdevice_ble_evt_handler_set(ble_evt_dispatch);
    APP_ERROR_CHECK(err_code);

    // Register with the SoftDevice handler module for BLE events.
    err_code = softdevice_sys_evt_handler_set(sys_evt_dispatch);
    APP_ERROR_CHECK(err_code);
}

// Build advertising data struct to pass into @ref ble_advertising_init.
static void setup_advdata(ble_advdata_t *advdata) {
  memset(advdata, 0, sizeof(*advdata));
  advdata->name_type          = BLE_ADVDATA_FULL_NAME;
  advdata->include_appearance = false;
  advdata->flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_LIMITED_DISC_MODE;
}


/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
    uint32_t      err_code;
    ble_advdata_t advdata;
    ble_advdata_t scanrsp;

    // Build advertising data struct to pass into @ref ble_advertising_init.
    setup_advdata(&advdata);

    memset(&scanrsp, 0, sizeof(scanrsp));
    scanrsp.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    scanrsp.uuids_complete.p_uuids  = m_adv_uuids;

    ble_adv_modes_config_t options = {0};
    options.ble_adv_fast_enabled  = BLE_ADV_FAST_ENABLED;
    options.ble_adv_fast_interval = APP_ADV_INTERVAL;
    options.ble_adv_fast_timeout  = APP_ADV_TIMEOUT_IN_SECONDS;

    err_code = ble_advertising_init(&advdata, &scanrsp, &options, NULL, NULL);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling the Device Manager events.
 *
 * @param[in] p_evt  Data associated to the device manager event.
 */
static uint32_t device_manager_evt_handler(dm_handle_t const * p_handle,
                                           dm_event_t const  * p_event,
                                           ret_code_t        event_result)
{
    APP_ERROR_CHECK(event_result);
    if (p_event->event_id == DM_EVT_LINK_SECURED)
    {
        //app_context_load(p_handle);
    }
    return NRF_SUCCESS;
}


/**@brief Function for the Device Manager initialization.
 *
 * @param[in] erase_bonds  Indicates whether bonding information should be cleared from
 *                         persistent storage during initialization of the Device Manager.
 */
static void device_manager_init(bool erase_bonds)
{
    uint32_t               err_code;
    dm_init_param_t        init_param = {.clear_persistent_data = erase_bonds};
    dm_application_param_t register_param;

    // Initialize persistent storage module.
    err_code = pstorage_init();
    APP_ERROR_CHECK(err_code);

    err_code = dm_init(&init_param);
    APP_ERROR_CHECK(err_code);

    memset(&register_param.sec_param, 0, sizeof(ble_gap_sec_params_t));

    register_param.sec_param.bond         = SEC_PARAM_BOND;
    register_param.sec_param.mitm         = SEC_PARAM_MITM;
    register_param.sec_param.io_caps      = SEC_PARAM_IO_CAPABILITIES;
    register_param.sec_param.oob          = SEC_PARAM_OOB;
    register_param.sec_param.min_key_size = SEC_PARAM_MIN_KEY_SIZE;
    register_param.sec_param.max_key_size = SEC_PARAM_MAX_KEY_SIZE;
    register_param.evt_handler            = device_manager_evt_handler;
    register_param.service_type           = DM_PROTOCOL_CNTXT_GATT_SRVR_ID;

    err_code = dm_register(&m_app_handle, &register_param);
    APP_ERROR_CHECK(err_code);
}


/*JSON{
    "type": "class",
    "class" : "NRF"
}
The NRF class is for controlling functionality of the Nordic nRF51/nRF52 chips. Currently these are only used in the [BBC micro:bit](/MicroBit).

The main part of this is control of Bluetooth Smart - both searching for devices, and changing advertising data.
*/
/*JSON{
  "type" : "object",
  "name" : "Bluetooth",
  "instanceof" : "Serial",
  "#ifdef" : "BLUETOOTH"
}
The Bluetooth Serial port - used when data is sent or received over Bluetooth Smart on nRF51/nRF52 chips.
 */
void jswrap_nrf_bluetooth_init(void) {
  // Initialize.
  APP_TIMER_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, false);
  ble_stack_init();
  
  bool erase_bonds;
  device_manager_init(erase_bonds);

  gap_params_init();
  services_init();
  advertising_init();
  conn_params_init();
  // ble_db_discovery_init(db_disc_handler); // FIXME

  jswrap_nrf_bluetooth_wake();
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "setName",
    "generate" : "jswrap_nrf_bluetooth_setName",
    "params" : [
      ["name","JsVar","The name to advertise as"]
    ]
}
Set the Name that will appear when another device searches
for Bluetooth devices.

**Note:** This clears any advertising data that was set - you'll
need to call `NRF.setAdvertising({...})` after to restore the data
if you had something set previously.
*/
void jswrap_nrf_bluetooth_setName(JsVar *name) {
    uint32_t                err_code;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    JSV_GET_AS_CHAR_ARRAY(namePtr, nameLen, name);
    if (!namePtr) return;

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)namePtr,
                                          nameLen);
    if (err_code)
      jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);

    jswrap_nrf_bluetooth_setAdvertising(0);
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "sleep",
    "generate" : "jswrap_nrf_bluetooth_sleep"
}
Disable Bluetooth communications
*/
void jswrap_nrf_bluetooth_sleep(void) {
  uint32_t err_code;

  // If connected, disconnect.
  if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
      err_code = sd_ble_gap_disconnect(m_conn_handle,  BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
      if (err_code)
          jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
  }

  // Stop advertising
  err_code = sd_ble_gap_adv_stop();
  NRF_RADIO->TASKS_DISABLE = (1UL);
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "wake",
    "generate" : "jswrap_nrf_bluetooth_wake"
}
Enable Bluetooth communications (they are enabled by default)
*/
void jswrap_nrf_bluetooth_wake(void) {
  NRF_RADIO->TASKS_DISABLE = (0UL);
  jswrap_nrf_bluetooth_startAdvertise();
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "getBattery",
    "generate" : "jswrap_nrf_bluetooth_getBattery",
    "return" : ["float", "Battery level in volts" ]
}
Get the battery level in volts
*/
JsVarFloat jswrap_nrf_bluetooth_getBattery(void) {
#ifdef NRF52
  return jshReadVRef();
#else
  // Configure ADC
  NRF_ADC->CONFIG     = (ADC_CONFIG_RES_8bit                        << ADC_CONFIG_RES_Pos)     |
                        (ADC_CONFIG_INPSEL_SupplyOneThirdPrescaling << ADC_CONFIG_INPSEL_Pos)  |
                        (ADC_CONFIG_REFSEL_VBG                      << ADC_CONFIG_REFSEL_Pos)  |
                        (ADC_CONFIG_PSEL_Disabled                   << ADC_CONFIG_PSEL_Pos)    |
                        (ADC_CONFIG_EXTREFSEL_None                  << ADC_CONFIG_EXTREFSEL_Pos);
  NRF_ADC->EVENTS_END = 0;
  NRF_ADC->ENABLE     = ADC_ENABLE_ENABLE_Enabled;

  NRF_ADC->EVENTS_END  = 0;    // Stop any running conversions.
  NRF_ADC->TASKS_START = 1;

  while (!NRF_ADC->EVENTS_END);

  uint16_t vbg_in_mv = 1200;
  uint8_t adc_max = 255;
  uint16_t vbat_current_in_mv = (NRF_ADC->RESULT * 3 * vbg_in_mv) / adc_max;

  NRF_ADC->EVENTS_END     = 0;
  NRF_ADC->TASKS_STOP     = 1;

  return vbat_current_in_mv / 1000.0;
#endif
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "setAdvertising",
    "generate" : "jswrap_nrf_bluetooth_setAdvertising",
    "params" : [
      ["data","JsVar","The data to advertise as an object - see below for more info"]
    ]
}
Change the data that Espruino advertises.

Data is of the form `{ UUID : data_as_byte_array }`. The UUID should be a [Bluetooth Service ID](https://developer.bluetooth.org/gatt/services/Pages/ServicesHome.aspx).

For example to return battery level at 95%, do:

```
NRF.setAdvertising({
  0x180F : [95]
});
```

Or you could report the current temperature:

```
setInterval(function() {
  NRF.setAdvertising({
    0x1809 : [Math,round(E.getTemperature())]
  });
}, 30000);
```
*/
void jswrap_nrf_bluetooth_setAdvertising(JsVar *data) {
  uint32_t err_code;
  ble_advdata_t advdata;
  setup_advdata(&advdata);

  if (jsvIsObject(data)) {
    ble_advdata_service_data_t *service_data = (ble_advdata_service_data_t*)alloca(jsvGetChildren(data)*sizeof(ble_advdata_service_data_t));
    int n = 0;
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, data);
    while (jsvObjectIteratorHasValue(&it)) {
      service_data[n].service_uuid = jsvGetIntegerAndUnLock(jsvObjectIteratorGetKey(&it));
      JsVar *v = jsvObjectIteratorGetValue(&it);
      JSV_GET_AS_CHAR_ARRAY(dPtr, dLen, v);
      jsvUnLock(v);
      service_data[n].data.size    = dLen;
      service_data[n].data.p_data  = (uint8_t*)dPtr;
      jsvObjectIteratorNext(&it);
      n++;
    }
    jsvObjectIteratorFree(&it);

    advdata.service_data_count   = n;
    advdata.p_service_data_array = service_data;
  } else if (!jsvIsUndefined(data)) {
    jsExceptionHere(JSET_TYPEERROR, "Expecting object or undefined, got %t", data);
  }

  err_code = ble_advdata_set(&advdata, NULL);
  if (err_code)
    jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
}


/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "setServices",
    "generate" : "jswrap_nrf_bluetooth_setServices",
    "params" : [
      ["data","JsVar","The service (and characteristics) to advertise"]
    ]
}
BETA: This only partially works at the moment

Change the services and characteristics Espruino advertises.

```
NRF.setServices({
  0xBCDE : {
    0xABCD : {
      value : "Hello", // optional
      maxLen : 5, // optional (otherwise is length of initial value)
      broadcast : false, // optional, default is false
      readable : true,   // optional, default is false
      writable : true,   // optional, default is false
      onWrite : function(evt) { // optional
        console.log("Got ", evt.data);
      }
    }
    // more characteristics allowed
  }
  // more services allowed
});
```
*/
void jswrap_nrf_bluetooth_setServices(JsVar *data) {
  uint32_t err_code;

  // TODO: Reset services

  if (jsvIsObject(data)) {
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, data);
    while (jsvObjectIteratorHasValue(&it)) {
      ble_uuid_t ble_uuid;
      uint16_t service_handle;

      // Add the service
      BLE_UUID_BLE_ASSIGN(ble_uuid, jsvGetIntegerAndUnLock(jsvObjectIteratorGetKey(&it)));
      err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                              &ble_uuid,
                                              &service_handle);
      if (err_code) {
        jsExceptionHere(JSET_ERROR, "Got BLE error code %d in gatts_service_add", err_code);
        break;
      }
      // sd_ble_gatts_include_add ?

      // Now add characteristics
      JsVar *serviceVar = jsvObjectIteratorGetValue(&it);
      JsvObjectIterator serviceit;
      jsvObjectIteratorNew(&serviceit, serviceVar);
      while (jsvObjectIteratorHasValue(&serviceit)) {
        ble_uuid_t          char_uuid;
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;
        ble_gatts_char_handles_t  characteristic_handles;

        BLE_UUID_BLE_ASSIGN(char_uuid, jsvGetIntegerAndUnLock(jsvObjectIteratorGetKey(&serviceit)));
        JsVar *charVar = jsvObjectIteratorGetValue(&serviceit);

        memset(&char_md, 0, sizeof(char_md));
        if (jsvGetBoolAndUnLock(jsvObjectGetChild(charVar, "broadcast", 0)))
          char_md.char_props.broadcast = 1;
        if (jsvGetBoolAndUnLock(jsvObjectGetChild(charVar, "readable", 0)))
          char_md.char_props.read = 1;
        if (jsvGetBoolAndUnLock(jsvObjectGetChild(charVar, "writable", 0))) {
          char_md.char_props.write = 1;
          char_md.char_props.write_wo_resp = 1;
        }
        char_md.p_char_user_desc         = NULL;
        char_md.p_char_pf                = NULL;
        char_md.p_user_desc_md           = NULL;
        char_md.p_cccd_md                = NULL;
        char_md.p_sccd_md                = NULL;

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
        attr_md.vloc       = BLE_GATTS_VLOC_STACK;
        attr_md.rd_auth    = 0;
        attr_md.wr_auth    = 0;
        attr_md.vlen       = 1; // TODO: variable length?

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid       = &char_uuid;
        attr_char_value.p_attr_md    = &attr_md;
        attr_char_value.init_len     = 0;
        attr_char_value.init_offs    = 0;
        attr_char_value.p_value      = 0;
        attr_char_value.max_len      = (uint16_t)jsvGetIntegerAndUnLock(jsvObjectGetChild(charVar, "maxLen", 0));
        if (attr_char_value.max_len==0) attr_char_value.max_len=1;

        // get initial data
        JsVar *charValue = jsvObjectGetChild(charVar, "value", 0);
        if (charValue) {
          JSV_GET_AS_CHAR_ARRAY(vPtr, vLen, charValue);
          if (vPtr && vLen) {
            attr_char_value.p_value = (uint8_t*)vPtr;
            attr_char_value.init_len = vLen;
            if (attr_char_value.init_len > attr_char_value.max_len)
              attr_char_value.max_len = attr_char_value.init_len;
          }
        }

        err_code = sd_ble_gatts_characteristic_add(service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &characteristic_handles);

        jsvUnLock(charValue); // unlock here in case we were storing data in a flat string
        if (err_code) {
          jsExceptionHere(JSET_ERROR, "Got BLE error code %d in gatts_characteristic_add", err_code);
          break;
        }

        // Add Write callback
        JsVar *writeCb = jsvObjectGetChild(charVar, "onWrite", 0);
        if (writeCb) {
          char eventName[12];
          ble_handle_to_write_event_name(eventName, characteristic_handles.value_handle);
          jsvObjectSetChildAndUnLock(execInfo.root, eventName, writeCb);
        }

        jsvUnLock(charVar);
        /* We'd update the characteristic with:

    memset(&hvx_params, 0, sizeof(hvx_params));
    hvx_params.handle = characteristic_handle.value_handle;
    hvx_params.p_data = p_string;
    hvx_params.p_len  = &length;
    hvx_params.type   = BLE_GATT_HVX_NOTIFICATION;
    return sd_ble_gatts_hvx(p_nus->conn_handle, &hvx_params);

    Maybe we could find the handle out based on characteristic UUID, rather than having
    to store it?

    */

        jsvObjectIteratorNext(&serviceit);
      }
      jsvObjectIteratorFree(&serviceit);
      jsvUnLock(serviceVar);

      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);



    } else if (!jsvIsUndefined(data)) {
      jsExceptionHere(JSET_TYPEERROR, "Expecting object or undefined, got %t", data);
    }
}


/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "setScan",
    "generate" : "jswrap_nrf_bluetooth_setScan",
    "params" : [
      ["callback","JsVar","The callback to call with information about received, or undefined to stop"]
    ]
}

Start/stop listening for BLE advertising packets within range.

```
// Start scanning
NRF.setScan(function(d) {
  console.log(JSON.stringify(d,null,2));
});
// prints {"rssi":-72, "addr":"##:##:##:##:##:##", "data":new ArrayBuffer([2,1,6,...])}

// Stop Scanning
NRF.setScan(false);
```
*/
void jswrap_nrf_bluetooth_setScan(JsVar *callback) {
  uint32_t              err_code;
  // set the callback event variable
  if (!jsvIsFunction(callback)) callback=0;
  jsvObjectSetChild(execInfo.root, BLE_SCAN_EVENT, callback);
  // either start or stop scanning
  if (callback) {
    ble_gap_scan_params_t     m_scan_param;
    // non-selective scan
    m_scan_param.active       = 0;            // Active scanning set.
    m_scan_param.selective    = 0;            // Selective scanning not set.
    m_scan_param.interval     = SCAN_INTERVAL;// Scan interval.
    m_scan_param.window       = SCAN_WINDOW;  // Scan window.
    m_scan_param.p_whitelist  = NULL;         // No whitelist provided.
    m_scan_param.timeout      = 0x0000;       // No timeout.

    err_code = sd_ble_gap_scan_start(&m_scan_param);
  } else {
    err_code = sd_ble_gap_scan_stop();
  }
  if (err_code)
    jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "setTxPower",
    "generate" : "jswrap_nrf_bluetooth_setTxPower",
    "params" : [
      ["power","int","Transmit power. Accepted values are -40, -30, -20, -16, -12, -8, -4, 0, and 4 dBm. Others will give an error code."]
    ]
}
Set the BLE radio transmit power. The default TX power is 0 dBm.
*/
void jswrap_nrf_bluetooth_setTxPower(JsVarInt pwr) {
  uint32_t              err_code;
  err_code = sd_ble_gap_tx_power_set(pwr);
  if (err_code)
    jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "connect",
    "generate" : "jswrap_nrf_bluetooth_connect",
    "params" : [
      ["mac","JsVar","The MAC address to connect to"]
    ]
}
Connect to a BLE device by MAC address

**Note:** This is only available on some devices
*/
void jswrap_nrf_bluetooth_connect(JsVar *mac) {
#if CENTRAL_LINK_COUNT>0
  // untested
  // Convert mac address to something readable - pretty hacky
  if (!jsvIsString(mac) ||
      jsvGetStringLength(mac)!=17 ||
      jsvGetCharInString(mac, 2)!=':' ||
      jsvGetCharInString(mac, 5)!=':' ||
      jsvGetCharInString(mac, 8)!=':' ||
      jsvGetCharInString(mac, 11)!=':' ||
      jsvGetCharInString(mac, 14)!=':') {
    jsExceptionHere(JSET_TYPEERROR, "Expecting a mac address of the form aa:bb:cc:dd:ee:ff");
    return;
  }
  ble_gap_addr_t peer_addr;
  peer_addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC; // not sure why this isn't public?
  int i;
  for (i=0;i<6;i++)
    peer_addr.addr[5-i] = (chtod(jsvGetCharInString(mac, i*3))<<4) | chtod(jsvGetCharInString(mac, (i*3)+1));
  uint32_t              err_code;
  ble_gap_scan_params_t     m_scan_param;
  m_scan_param.active       = 0;            // Active scanning set.
  m_scan_param.selective    = 0;            // Selective scanning not set.
  m_scan_param.interval     = SCAN_INTERVAL;// Scan interval.
  m_scan_param.window       = SCAN_WINDOW;  // Scan window.
  m_scan_param.p_whitelist  = NULL;         // No whitelist provided.
  m_scan_param.timeout      = 0x0000;       // No timeout.

  ble_gap_conn_params_t   gap_conn_params;
  gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
  gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
  gap_conn_params.slave_latency     = SLAVE_LATENCY;
  gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

  err_code = sd_ble_gap_connect(&peer_addr, &m_scan_param, &gap_conn_params);
  if (err_code)
    jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
#else
  jsExceptionHere(JSET_ERROR, "Unimplemented");
#endif
}

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "disconnect",
    "generate" : "jswrap_nrf_bluetooth_disconnect"
}
Connect to a BLE device by MAC address

**Note:** This is only available on some devices
*/
void jswrap_nrf_bluetooth_disconnect() {
#if CENTRAL_LINK_COUNT>0
  uint32_t              err_code;

  if (m_central_conn_handle != BLE_CONN_HANDLE_INVALID) {
    // we have a connection, disconnect
    err_code = sd_ble_gap_disconnect(m_central_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
  } else {
    // no connection - try and cancel the connect attempt (assume we have one)
    err_code = sd_ble_gap_connect_cancel();
  }
  if (err_code) {
    jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
  }
#else
  jsExceptionHere(JSET_ERROR, "Unimplemented");
#endif
}


#if CENTRAL_LINK_COUNT>0
/**@brief Function for handling database discovery events.
 *
 * @details This function is callback function to handle events from the database discovery module.
 *          Depending on the UUIDs that are discovered, this function should forward the events
 *          to their respective services.
 *
 * @param[in] p_event  Pointer to the database discovery event.
 */
static void db_disc_handler(ble_db_discovery_evt_t * p_evt) {
  if (p_evt->evt_type == BLE_DB_DISCOVERY_COMPLETE) {
    ble_gatt_db_srv_t *srv = &p_evt->params.discovered_db;
    jsiConsolePrintf("UUID 0x%04x cnt %d\n", srv->srv_uuid, srv->char_count);
  }
}
#endif

/*JSON{
    "type" : "staticmethod",
    "class" : "NRF",
    "name" : "discoverAllServicesAndCharacteristics",
    "generate" : "jswrap_nrf_bluetooth_discoverAllServicesAndCharacteristics"
}
Connect to a BLE device by MAC address

**Note:** This is only available on some devices
*/
void jswrap_nrf_bluetooth_discoverAllServicesAndCharacteristics() {
#if CENTRAL_LINK_COUNT>0
  // doing direct - or maybe see ble_app_blinky_c?
  if (m_central_conn_handle == BLE_CONN_HANDLE_INVALID) {
    jsExceptionHere(JSET_ERROR, "Not Connected");
  }

  uint32_t              err_code;
  err_code = ble_db_discovery_start(&m_ble_db_discovery, m_central_conn_handle);
  if (err_code)
    jsExceptionHere(JSET_ERROR, "Got BLE error code %d", err_code);
#else
  jsExceptionHere(JSET_ERROR, "Unimplemented");
#endif
}

/*JSON{
  "type" : "idle",
  "generate" : "jswrap_nrf_idle"
}*/
bool jswrap_nrf_idle() {
  return jswrap_nrf_transmit_string()>0; // return true if we sent anything
}

/*JSON{
  "type" : "kill",
  "generate" : "jswrap_nrf_kill"
}*/
void jswrap_nrf_kill() {
  // if we were scanning, make sure we stop at reset!
  if (bleStatus & BLE_IS_SCANNING) {
    sd_ble_gap_scan_stop();
    bleStatus &= ~BLE_IS_SCANNING;
  }
}


