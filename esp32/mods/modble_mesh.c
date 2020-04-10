/*
 * Copyright (c) 2020, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */

#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/gc.h"
#include <string.h>
#include <stdbool.h>

#include "esp_bt_device.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_networking_api.h"

#include "mpirq.h"

/******************************************************************************
 DEFINE CONSTANTS
 ******************************************************************************/
#define MOD_BLE_MESH_SERVER              (0)
#define MOD_BLE_MESH_CLIENT              (1)

#define MOD_BLE_MESH_SERVER              (0)
#define MOD_BLE_MESH_CLIENT              (1)

#define MOD_BLE_MESH_PROV_ADV            (1)
#define MOD_BLE_MESH_PROV_GATT           (2)
#define MOD_BLE_MESH_PROV_NONE           (4)

#define MOD_BLE_MESH_INPUT_OOB           (1)
#define MOD_BLE_MESH_OUTPUT_OOB          (2)

#define MOD_BLE_MESH_RELAY               (1)
#define MOD_BLE_MESH_GATT_PROXY          (2)
#define MOD_BLE_MESH_LOW_POWER           (4)
#define MOD_BLE_MESH_FRIEND              (8)

#define MOD_BLE_MESH_MODEL_GENERIC       (0)
#define MOD_BLE_MESH_MODEL_SENSORS       (1)
#define MOD_BLE_MESH_MODEL_TIME_SCENES   (2)
#define MOD_BLE_MESH_MODEL_LIGHTNING     (3)

#define MOD_BLE_MESH_MODEL_ONOFF         (0)
#define MOD_BLE_MESH_MODEL_LEVEL         (1)

#define MOD_BLE_MESH_STATE_BOOL          (0)
#define MOD_BLE_MESH_STATE_INT           (1)

#define MOD_BLE_MESH_DEFAULT_NAME        "PYCOM-ESP-BLE-MESH"

#define MOD_BLE_MESH_STATE_LOCATION_IN_SRV_T_TYPE (sizeof(esp_ble_mesh_model_t*) + sizeof(esp_ble_mesh_server_rsp_ctrl_t))


/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/

// There is only one UUID, and it cannot be changed during runtime
// TODO: why is this initialized with these 2 values ?
static uint8_t dev_uuid[16] = { 0xdd, 0xdd };

// This is the same as esp_ble_mesh_elem_t just without const members
typedef struct mod_ble_mesh_elem_s {
    uint16_t element_addr;
    uint16_t location;
    uint8_t sig_model_count;
    uint8_t vnd_model_count;
    esp_ble_mesh_model_t *sig_models;
    esp_ble_mesh_model_t *vnd_models;
}mod_ble_mesh_elem_t;

typedef struct mod_ble_mesh_element_class_s {
    mp_obj_base_t base;
    mod_ble_mesh_elem_t *element;
}mod_ble_mesh_element_class_t;

typedef struct mod_ble_mesh_model_class_s {
    mp_obj_base_t base;
    struct mod_ble_mesh_model_class_s* next;
    // Pointer to the element owns this model
    mod_ble_mesh_element_class_t *element;
    // List of Application Index :
    // Index in the esp ble context
    uint8_t index;
    // User defined MicroPython callback function
    mp_obj_t callback;
    // TODO: check whether this value has any sense
    // MicroPython object for the value of the model
    mp_obj_t value;
}mod_ble_mesh_model_class_t;

typedef struct mod_ble_mesh_generic_server_callback_param_s {
    esp_ble_mesh_generic_server_cb_event_t event;
    esp_ble_mesh_generic_server_cb_param_t* param;
}mod_ble_mesh_generic_server_callback_param_t;

typedef struct mod_ble_mesh_generic_client_callback_param_s {
    esp_ble_mesh_generic_client_cb_event_t event;
    esp_ble_mesh_generic_client_cb_param_t* param;
}mod_ble_mesh_generic_client_callback_param_t;

typedef struct mod_ble_mesh_provision_callback_param_s {
    mp_obj_t callback;
    int8_t oob_key;
    int8_t oob_type;
}mod_ble_mesh_provision_callback_param_t;

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/

/******************************************************************************
 DEFINE PRIVATE VARIABLES
 ******************************************************************************/

// TYPE : SIZE table, can be addressed with (opcode & 0x00FF).
static const uint8_t generic_type_and_size_table[][2] = {
        // Dummy entry because Opcode starts with 1 instead of 0...
        {0, 0},
        // GENERIC ONOFF
        {MOD_BLE_MESH_STATE_BOOL,  sizeof(esp_ble_mesh_server_recv_gen_onoff_set_t)},
        {MOD_BLE_MESH_STATE_BOOL,  sizeof(esp_ble_mesh_server_recv_gen_onoff_set_t)},
        {MOD_BLE_MESH_STATE_BOOL,  sizeof(esp_ble_mesh_server_recv_gen_onoff_set_t)},
        {MOD_BLE_MESH_STATE_BOOL,  sizeof(esp_ble_mesh_state_change_gen_onoff_set_t)},
        // GENERIC LEVEL
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_server_recv_gen_level_set_t)},
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_server_recv_gen_level_set_t)},
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_server_recv_gen_level_set_t)},
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_state_change_gen_level_set_t)},
        // GENERIC DELTA
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_server_recv_gen_delta_set_t)},
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_state_change_gen_delta_set_t)},
        // GENERIC MOVE
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_server_recv_gen_move_set_t)},
        {MOD_BLE_MESH_STATE_INT,  sizeof(esp_ble_mesh_state_change_gen_move_set_t)},
};


static const mp_obj_type_t mod_ble_mesh_model_type;
// TODO: add support for more Elements
static mod_ble_mesh_element_class_t mod_ble_mesh_element;
// This is a list containing all the Models regardless their Elements
static mod_ble_mesh_model_class_t *mod_ble_models_list = NULL;

static bool initialized = false;
static esp_ble_mesh_prov_t *provision_ptr;
static esp_ble_mesh_comp_t *composition_ptr;

// Find a better place for definition
mp_obj_t provision_callback;

/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/

static mod_ble_mesh_model_class_t* mod_ble_add_model_to_list(mod_ble_mesh_element_class_t* ble_mesh_element,
                                                             uint8_t index,
                                                             mp_obj_t callback,
                                                             mp_obj_t value) {

    mod_ble_mesh_model_class_t *model = mod_ble_models_list;
    mod_ble_mesh_model_class_t *model_last = NULL; // Only needed as helper pointer for the case when the list is fully empty


    // Find the last element of the list
    while(model != NULL) {
        // Save the last Model we checked
        model_last = model;
        // Jump to the next Model
        model = model->next;
    }

    // Allocate memory for the new element
    //TODO: check this allocation, it should be from GC controlled memory
    model = (mod_ble_mesh_model_class_t*)heap_caps_malloc(sizeof(mod_ble_mesh_model_class_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Initialize the new Model
    model->base.type = &mod_ble_mesh_model_type;
    model->element = ble_mesh_element;
    model->index = index;
    model->callback = callback;
    model->value = value;
    model->next = NULL;

    // Add the new element to the list
    if(model_last != NULL) {
        model_last->next = model;
    }
    else {
        // This means this new element is the very first one in the list
        mod_ble_models_list = model;
    }

    return model;
}

static mod_ble_mesh_model_class_t* mod_ble_find_model(esp_ble_mesh_model_t *esp_ble_model) {

    mod_ble_mesh_model_class_t *model = mod_ble_models_list;

    // TODO: handle when we have more elements
    // Iterate through on all the Models
    while(model != NULL) {
        if(model->index == esp_ble_model->model_idx) {
            return model;
        }
        model = model->next;
    }

    return NULL;
}

// TODO: check what other arguments are needed, or whether these arguments are needed at all
static void mod_ble_mesh_generic_server_callback_call_mp_callback(mp_obj_t callback,
                                                                  esp_ble_mesh_generic_server_cb_event_t event,
                                                                  uint32_t recv_op,
                                                                  void* value,
                                                                  uint32_t value_type
                                                                  ){
    // Call the registered function
    if(callback != NULL) {
        mp_obj_t args[3];
        args[0] = mp_obj_new_int(event);
        //TODO: check what other object should be passed from the context
        args[1] = mp_obj_new_int(recv_op);

        switch (value_type) {
            case MOD_BLE_MESH_STATE_BOOL:
                args[2] = mp_obj_new_bool(*((mp_int_t*)value));
                break;
            case MOD_BLE_MESH_STATE_INT:
                args[2] = mp_obj_new_int(*((mp_int_t*)value));
                break;
            default:
                // If the type is unknown return with None
                args[2] = mp_const_none;
        }

        // At this point we need to free up the value, it is already stored as a MicroPython object
        heap_caps_free(value);

        // TODO: check if these 3 arguments are really needed here
        mp_call_function_n_kw(callback, 3, 0, args);
    }
}

static void mod_ble_mesh_generic_server_callback_handler(void* param_in) {

    mod_ble_mesh_generic_server_callback_param_t* callback_param = (mod_ble_mesh_generic_server_callback_param_t*)param_in;
    mod_ble_mesh_model_class_t* mod_ble_model = mod_ble_find_model(callback_param->param->model);

    if(mod_ble_model != NULL) {

        esp_ble_mesh_generic_server_cb_event_t event = callback_param->event;
        esp_ble_mesh_generic_server_cb_param_t* param = callback_param->param;
        esp_ble_mesh_model_t* model = callback_param->param->model;
        uint8_t type = 0;
        uint16_t size = 0;
        uint8_t* data_ptr = NULL;

        switch (event) {
            case ESP_BLE_MESH_GENERIC_SERVER_RECV_SET_MSG_EVT:
                // TODO: implement this case, for now let if falling through to STATE because
                // ESP_BLE_MESH_SERVER_AUTO_RSP is set, so SET event will never come, only STATE
            case ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT:
                type =  generic_type_and_size_table[param->ctx.recv_op & 0x00FF][0];
                size = generic_type_and_size_table[param->ctx.recv_op & 0x00FF][1];
                data_ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                memcpy(data_ptr, &param->value.state_change, size);

                //TODO: this function might be called from mod_ble_mesh_generic_server_callback()
                //TODO: double check whether this is needed when ESP_BLE_MESH_SERVER_AUTO_RSP is set
                // Publish that the State of this Server Model changed
                esp_ble_mesh_model_publish(model, type, size, data_ptr, ROLE_NODE);

                // Call the registered MP function
                mod_ble_mesh_generic_server_callback_call_mp_callback(mod_ble_model->callback,
                                                                      event,
                                                                      param->ctx.recv_op,
                                                                      data_ptr,
                                                                      type);
                break;
            case ESP_BLE_MESH_GENERIC_SERVER_RECV_GET_MSG_EVT:
                size = generic_type_and_size_table[param->ctx.recv_op & 0x00FF][1];
                // The data behind the model->user_data always starts at a given location, regardless of the type of the Model
                // Copy the actual value
                memcpy(&data_ptr, ((uint8_t*)callback_param->param->model->user_data) + MOD_BLE_MESH_STATE_LOCATION_IN_SRV_T_TYPE, sizeof(uint8_t*));
                esp_ble_mesh_server_model_send_msg(model,
                                                   &callback_param->param->ctx,
                                                   param->ctx.recv_op,
                                                   size,
                                                   data_ptr);
                break;
            default:
                // Handle it silently
                break;
        }
    }
}

//// TODO: check what other arguments are needed, or whether these arguments are needed at all
//static void mod_ble_mesh_generic_client_callback_call_mp_callback(mp_obj_t callback,
//                                                                  esp_ble_mesh_generic_server_cb_event_t event,
//                                                                  uint32_t recv_op,
//                                                                  void* value,
//                                                                  uint32_t value_type
//                                                                  ){
//    // Call the registered function
//    if(callback != NULL) {
//        mp_obj_t args[3];
//        args[0] = mp_obj_new_int(event);
//        //TODO: check what other object should be passed from the context
//        args[1] = mp_obj_new_int(recv_op);
//
//        switch (value_type) {
//            case MOD_BLE_MESH_STATE_BOOL:
//                args[2] = mp_obj_new_bool(*((mp_int_t*)value));
//                break;
//            case MOD_BLE_MESH_STATE_INT:
//                args[2] = mp_obj_new_int(*((mp_int_t*)value));
//                break;
//            default:
//                // If the type is unknown return with None
//                args[2] = mp_const_none;
//        }
//
//        // At this point we need to free up the value, it is already stored as a MicroPython object
//        heap_caps_free(value);
//
//        // TODO: check if these 3 arguments are really needed here
//        mp_call_function_n_kw(callback, 3, 0, args);
//    }
//}

static void mod_ble_mesh_generic_client_callback_handler(void* param_in) {

    mod_ble_mesh_generic_client_callback_param_t* callback_param = (mod_ble_mesh_generic_client_callback_param_t*)param_in;
    mod_ble_mesh_model_class_t* mod_ble_model = mod_ble_find_model(callback_param->param->params->model);

    if(mod_ble_model != NULL) {

        esp_ble_mesh_generic_client_cb_event_t event = callback_param->event;
        esp_ble_mesh_generic_client_cb_param_t* param = callback_param->param;
       // esp_ble_mesh_model_t* model = callback_param->param->params->model;

        switch (event) {
            case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
               printf("ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT\n");
                if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
                    printf("ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET, onoff %d\n", param->status_cb.onoff_status.present_onoff);
                }
                break;
            case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
                printf("ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT\n");
                if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
                    printf("ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET, onoff %d\n", param->status_cb.onoff_status.present_onoff);
                }
                break;
            case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
                printf("ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT\n");
                break;
            case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
                printf("ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT\n");
                break;
            default:
                break;
            }
    }
}

STATIC void mod_ble_mesh_provision_callback_call_mp_callback(void* param_in) {

    mod_ble_mesh_provision_callback_param_t* callback_param = (mod_ble_mesh_provision_callback_param_t*)param_in;
    mp_obj_t args[2];
    // GET OOB Type
    args[0] = mp_obj_new_int(callback_param->oob_type);

    if(callback_param->oob_type == MOD_BLE_MESH_OUTPUT_OOB) {
        //OOB Key
        args[1] = mp_obj_new_int(callback_param->oob_key);
    }
    else if(callback_param->oob_type == MOD_BLE_MESH_INPUT_OOB) {
        //Invalid OOB Key
        args[1] = mp_const_none;
    }

    // Call the user registered MicroPython function
    mp_call_function_n_kw(callback_param->callback, 2, 0, args);

    // Free callback_param
    heap_caps_free(callback_param);
}

static void mod_ble_mesh_provision_callback(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    // TODO: here the user registered MicroPython API should be called with correct parameters via the Interrupt Task
    printf("mod_ble_mesh_provision_callback is called: %d\n", event);

    switch (event) {
        case ESP_BLE_MESH_NODE_PROV_OUTPUT_NUMBER_EVT:
            if(provision_callback != NULL) {

                mod_ble_mesh_provision_callback_param_t* callback_param_ptr = (mod_ble_mesh_provision_callback_param_t *)heap_caps_malloc(sizeof(mod_ble_mesh_provision_callback_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                callback_param_ptr->callback = provision_callback;
                callback_param_ptr->oob_type = MOD_BLE_MESH_OUTPUT_OOB;
                callback_param_ptr->oob_key = param->node_prov_output_num.number;

                // The user registered MicroPython callback will be called decoupled from the BLE Mesh context in the IRQ Task
                mp_irq_queue_interrupt(mod_ble_mesh_provision_callback_call_mp_callback, (void *)callback_param_ptr);
            }
            break;
        case ESP_BLE_MESH_NODE_PROV_INPUT_EVT:
            if(provision_callback != NULL) {

                mod_ble_mesh_provision_callback_param_t* callback_param_ptr = (mod_ble_mesh_provision_callback_param_t *)heap_caps_malloc(sizeof(mod_ble_mesh_provision_callback_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                callback_param_ptr->callback = provision_callback;
                callback_param_ptr->oob_type = MOD_BLE_MESH_INPUT_OOB;

                // The user registered MicroPython callback will be called decoupled from the BLE Mesh context in the IRQ Task
                mp_irq_queue_interrupt(mod_ble_mesh_provision_callback_call_mp_callback, (void *)callback_param_ptr);
            }
            break;
        case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
            printf("ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d\n", param->prov_register_comp.err_code);
            break;
        case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
            printf("ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT, err_code %d\n", param->node_prov_enable_comp.err_code);
            break;
        case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
            printf("ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT, bearer %s\n",
                param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
            break;
        case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
            printf("ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT, bearer %s\n",
                param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
            break;
        case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
            printf("ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT\n");
            break;
        case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
            printf("ESP_BLE_MESH_NODE_PROV_RESET_EVT\n");
            break;
        case ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT:
            printf("ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT, err_code %d\n", param->node_set_unprov_dev_name_comp.err_code);
            break;
        default:
            break;
        }
}

static void mod_ble_mesh_generic_client_callback(esp_ble_mesh_generic_client_cb_event_t event, esp_ble_mesh_generic_client_cb_param_t *param) {

    // TODO: here the user registered MicroPython API should be called with correct parameters via the Interrupt Task
    mod_ble_mesh_generic_client_callback_param_t* callback_param_ptr = (mod_ble_mesh_generic_client_callback_param_t *)heap_caps_malloc(sizeof(mod_ble_mesh_generic_server_callback_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    callback_param_ptr->param = (esp_ble_mesh_generic_client_cb_param_t *)heap_caps_malloc(sizeof(esp_ble_mesh_generic_client_cb_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    callback_param_ptr->param->params = (esp_ble_mesh_client_common_param_t *)heap_caps_malloc(sizeof(esp_ble_mesh_client_common_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);


    callback_param_ptr->event = event;
    memcpy(callback_param_ptr->param, param, sizeof(esp_ble_mesh_generic_client_cb_param_t));
    memcpy(callback_param_ptr->param->params, param->params, sizeof(esp_ble_mesh_client_common_param_t));

    // The registered callback will be handled in context of TASK_Interrupts
    mp_irq_queue_interrupt_non_ISR(mod_ble_mesh_generic_client_callback_handler, (void *)callback_param_ptr);

}

static void mod_ble_mesh_generic_server_callback(esp_ble_mesh_generic_server_cb_event_t event, esp_ble_mesh_generic_server_cb_param_t *param) {

    mod_ble_mesh_generic_server_callback_param_t* callback_param_ptr = (mod_ble_mesh_generic_server_callback_param_t *)heap_caps_malloc(sizeof(mod_ble_mesh_generic_server_callback_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    callback_param_ptr->param = (esp_ble_mesh_generic_server_cb_param_t *)heap_caps_malloc(sizeof(esp_ble_mesh_generic_server_cb_param_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    callback_param_ptr->event = event;
    memcpy(callback_param_ptr->param, param, sizeof(esp_ble_mesh_generic_server_cb_param_t));

    // The registered callback will be handled in context of TASK_Interrupts
    mp_irq_queue_interrupt_non_ISR(mod_ble_mesh_generic_server_callback_handler, (void *)callback_param_ptr);

}

static void mod_ble_mesh_config_server_callback(esp_ble_mesh_cfg_server_cb_event_t event, esp_ble_mesh_cfg_server_cb_param_t *param) {

    // TODO: here the user registered MicroPython API should be called with correct parameters via the Interrupt Task

    printf("Config Server callback, event: %d\n", event);

    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        switch (param->ctx.recv_op) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            printf("ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD\n");
            printf("net_idx 0x%04x, app_idx 0x%04x\n",
                param->value.state_change.appkey_add.net_idx,
                param->value.state_change.appkey_add.app_idx);
            ESP_LOG_BUFFER_HEX("AppKey", param->value.state_change.appkey_add.app_key, 16);

            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            printf("ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND\n");
            printf("elem_addr 0x%04x, app_idx 0x%04x, cid 0x%04x, mod_id 0x%04x\n",
                param->value.state_change.mod_app_bind.element_addr,
                param->value.state_change.mod_app_bind.app_idx,
                param->value.state_change.mod_app_bind.company_id,
                param->value.state_change.mod_app_bind.model_id);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
            printf("ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD\n");
            printf("elem_addr 0x%04x, sub_addr 0x%04x, cid 0x%04x, mod_id 0x%04x\n",
                param->value.state_change.mod_sub_add.element_addr,
                param->value.state_change.mod_sub_add.sub_addr,
                param->value.state_change.mod_sub_add.company_id,
                param->value.state_change.mod_sub_add.model_id);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET:
            printf("ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET\n");
            printf("elem_addr 0x%04x, pub_addr 0x%04x, pub_period %d, mod_id 0x%04x\n",
                    param->value.state_change.mod_pub_set.element_addr,
                    param->value.state_change.mod_pub_set.pub_addr,
                    param->value.state_change.mod_pub_set.pub_period,
                    param->value.state_change.mod_pub_set.model_id);
            break;
        default:
            //TODO: we need to handle all types of event coming from Configuration Client (Provisioner) !
            break;
        }
    }
}

/******************************************************************************
 DEFINE BLE MESH MODEL FUNCTIONS
 ******************************************************************************/

static uint8_t msg_tid = 0x0;

STATIC mp_obj_t mod_ble_mesh_model_set_state(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    STATIC const mp_arg_t mod_ble_mesh_model_set_state_args[] = {
            { MP_QSTR_self_in,               MP_ARG_OBJ,                  },
            { MP_QSTR_addr,                  MP_ARG_INT,                  },
            { MP_QSTR_value,                 MP_ARG_INT,                  },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_ble_mesh_model_set_state_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_ble_mesh_model_set_state_args, args);

    mod_ble_mesh_model_class_t* ble_mesh_model = (mod_ble_mesh_model_class_t*)args[0].u_obj;
    mp_int_t addr = args[1].u_int;
    mp_int_t value = args[2].u_int;

    esp_ble_mesh_generic_client_set_state_t set = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    esp_err_t err;

    common.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK;
    common.model = &ble_mesh_model->element->element->sig_models[ble_mesh_model->index];
    // TODO: set the correct netidx here
    common.ctx.net_idx = 0;
    // TODO: set the correct netidx here
    common.ctx.app_idx = 0;
    common.ctx.addr = addr;
    common.ctx.send_ttl = 3;
    common.ctx.send_rel = false;
    common.msg_timeout = 0;     /* 0 indicates that timeout value from menuconfig will be used */
    common.msg_role = ROLE_NODE;

    set.onoff_set.op_en = false;
    set.onoff_set.onoff = value == 0 ? false : true;
    set.onoff_set.tid = msg_tid++;

    err = esp_ble_mesh_generic_client_set_state(&common, &set);
    if (err) {
        printf("esp_ble_mesh_generic_client_set_state returned: %d\n", err);
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ble_mesh_model_set_state_obj, 3, mod_ble_mesh_model_set_state);


STATIC const mp_map_elem_t mod_ble_mesh_model_locals_dict_table[] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                        MP_OBJ_NEW_QSTR(MP_QSTR_BLE_Mesh_Model) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_set_state),                       (mp_obj_t)&mod_ble_mesh_model_set_state_obj },

};
STATIC MP_DEFINE_CONST_DICT(mod_ble_mesh_model_locals_dict, mod_ble_mesh_model_locals_dict_table);

static const mp_obj_type_t mod_ble_mesh_model_type = {
    { &mp_type_type },
    .name = MP_QSTR_BLE_Mesh_Model,
    .locals_dict = (mp_obj_t)&mod_ble_mesh_model_locals_dict,
};
/******************************************************************************
 DEFINE BLE MESH ELEMENT FUNCTIONS
 ******************************************************************************/

STATIC mp_obj_t mod_ble_mesh_element_add_model(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    STATIC const mp_arg_t mod_ble_mesh_add_model_args[] = {
            { MP_QSTR_self_in,               MP_ARG_OBJ,                                                       },
            { MP_QSTR_type,                  MP_ARG_INT,                  {.u_int = MOD_BLE_MESH_MODEL_GENERIC}},
            { MP_QSTR_functionality,         MP_ARG_INT,                  {.u_int = MOD_BLE_MESH_MODEL_ONOFF}},
            { MP_QSTR_server_client,         MP_ARG_INT,                  {.u_int = MOD_BLE_MESH_SERVER}},
            { MP_QSTR_callback,              MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}},
            { MP_QSTR_value,                 MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_ble_mesh_add_model_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_ble_mesh_add_model_args, args);

    mod_ble_mesh_element_class_t* ble_mesh_element = (mod_ble_mesh_element_class_t*)args[0].u_obj;
    mp_int_t type = args[1].u_int;
    mp_int_t func = args[2].u_int;
    mp_int_t server_client = args[3].u_int;
    mp_obj_t callback = args[4].u_obj;
    mp_obj_t value = args[5].u_obj;

    // Start preparing the Model
    esp_ble_mesh_model_t tmp_model;
    // Server Type
    if(server_client == MOD_BLE_MESH_SERVER) {
        if(type == MOD_BLE_MESH_MODEL_GENERIC) {
            if(func == MOD_BLE_MESH_MODEL_ONOFF) {
                //TODO: check this publication structure
                ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_srv_pub, 2 + 3, ROLE_NODE);
                // This will be saved into the Model's user_data field
                esp_ble_mesh_gen_onoff_srv_t* onoff_server = (esp_ble_mesh_gen_onoff_srv_t *)heap_caps_malloc(sizeof(esp_ble_mesh_gen_onoff_srv_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                onoff_server->rsp_ctrl.get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP;
                onoff_server->rsp_ctrl.set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP;

                esp_ble_mesh_model_t gen_onoff_srv_mod = ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_srv_pub, onoff_server);
                memcpy(&tmp_model, &gen_onoff_srv_mod, sizeof(gen_onoff_srv_mod));
            }
            else if(func == MOD_BLE_MESH_MODEL_LEVEL) {
                //TODO: check this publication structure
                ESP_BLE_MESH_MODEL_PUB_DEFINE(level_srv_pub, 2 + 3, ROLE_NODE);
                // This will be saved into the Model's user_data field
                esp_ble_mesh_gen_level_srv_t* level_server = (esp_ble_mesh_gen_level_srv_t *)heap_caps_malloc(sizeof(esp_ble_mesh_gen_level_srv_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                level_server->rsp_ctrl.get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP;
                level_server->rsp_ctrl.set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP;

                esp_ble_mesh_model_t gen_level_srv_mod = ESP_BLE_MESH_MODEL_GEN_LEVEL_SRV(&level_srv_pub, level_server);
                memcpy(&tmp_model, &gen_level_srv_mod, sizeof(gen_level_srv_mod));
            }
            else {
                //TODO: Add support for more functionality
            }
        }
        else {
            //TODO: Add support for more types
        }
    }
    // Client Type
    else {
        if(type == MOD_BLE_MESH_MODEL_GENERIC) {
            if(func == MOD_BLE_MESH_MODEL_ONOFF) {
                //TODO: check this publication structure
                ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 2 + 1, ROLE_NODE);
                // This will be saved into the Model's user_data field
                esp_ble_mesh_client_t* onoff_client = (esp_ble_mesh_client_t *)heap_caps_calloc(1, sizeof(esp_ble_mesh_client_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                esp_ble_mesh_model_t gen_onoff_cli_mod = ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, onoff_client);
                memcpy(&tmp_model, &gen_onoff_cli_mod, sizeof(gen_onoff_cli_mod));
            }
            else {
                //TODO: Add support for more functionality
            }
        }
        else {
            //TODO: Add support for more types
        }
    }

    // Allocate memory for the new model, use realloc because the underlying BLE Mesh library expects the Models as an array, and not as a list
    ble_mesh_element->element->sig_models = (esp_ble_mesh_model_t*)heap_caps_realloc(ble_mesh_element->element->sig_models, (ble_mesh_element->element->sig_model_count+1) * sizeof(esp_ble_mesh_model_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    memcpy(&ble_mesh_element->element->sig_models[ble_mesh_element->element->sig_model_count], &tmp_model, sizeof(tmp_model));

    // Create the MicroPython Model
    mod_ble_mesh_model_class_t* model = mod_ble_add_model_to_list(ble_mesh_element,
                                                                 ble_mesh_element->element->sig_model_count,
                                                                 callback,
                                                                 value);

    // Indicate we have a new element in the array
    ble_mesh_element->element->sig_model_count++;

    // Return with the MicroPtyhon Model object
    return model;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ble_mesh_element_add_model_obj, 1, mod_ble_mesh_element_add_model);

STATIC const mp_map_elem_t mod_ble_mesh_element_locals_dict_table[] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                     MP_OBJ_NEW_QSTR(MP_QSTR_BLE_Mesh_Element) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_add_model),                    (mp_obj_t)&mod_ble_mesh_element_add_model_obj },

};
STATIC MP_DEFINE_CONST_DICT(mod_ble_mesh_element_locals_dict, mod_ble_mesh_element_locals_dict_table);

static const mp_obj_type_t mod_ble_mesh_element_type = {
    { &mp_type_type },
    .name = MP_QSTR_BLE_Mesh_Element,
    .locals_dict = (mp_obj_t)&mod_ble_mesh_element_locals_dict,
};


/******************************************************************************
 DEFINE BLE MESH CLASS FUNCTIONS
 ******************************************************************************/

// TODO: add parameters
// Initialize the module
STATIC mp_obj_t mod_ble_mesh_init(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    STATIC const mp_arg_t mod_ble_mesh_init_args[] = {
            { MP_QSTR_name,                  MP_ARG_OBJ,                  {.u_obj = MP_OBJ_NULL}},
            { MP_QSTR_auth,                  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_ble_mesh_init_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_ble_mesh_init_args, args);

    // The BLE Mesh module should be initialized only once
    if(initialized == false) {
        esp_err_t err;

        provision_ptr = (esp_ble_mesh_prov_t *)heap_caps_malloc(sizeof(esp_ble_mesh_prov_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        composition_ptr = (esp_ble_mesh_comp_t *)heap_caps_malloc(sizeof(esp_ble_mesh_comp_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        if(provision_ptr != NULL && composition_ptr != NULL) {

            // Set UUID
            memcpy(dev_uuid + 2, esp_bt_dev_get_address(), BD_ADDR_LEN);
            provision_ptr->uuid = dev_uuid;

            //TODO: Support multiple auth metods paralelly
            // Initiate params with NO OOB case
            provision_ptr->input_size = 0;
            provision_ptr->input_actions = ESP_BLE_MESH_NO_INPUT;

            provision_ptr->output_size = 0;
            provision_ptr->output_actions = ESP_BLE_MESH_NO_OUTPUT;

            // GET auth information
            int oob_type = args[1].u_int;

            if(oob_type & MOD_BLE_MESH_INPUT_OOB) {
                provision_ptr->input_size = 1;
                provision_ptr->input_actions = ESP_BLE_MESH_PUSH;
            }
            if(oob_type & MOD_BLE_MESH_OUTPUT_OOB) {
                provision_ptr->output_size = 1;
                provision_ptr->output_actions = ESP_BLE_MESH_BLINK;
            }

            //TODO: initialize composition based on input parameters
            composition_ptr->cid = 0x02C4;  // CID_ESP=0x02C4
            // TODO: add support for more Elements
            // TODO: check this cast, it might not be good
            composition_ptr->elements = (esp_ble_mesh_elem_t *)mod_ble_mesh_element.element;
            composition_ptr->element_count = 1;

            // TODO: all kind of callbacks should be registered here
            esp_ble_mesh_register_generic_client_callback(mod_ble_mesh_generic_client_callback);
            esp_ble_mesh_register_generic_server_callback(mod_ble_mesh_generic_server_callback);
            esp_ble_mesh_register_config_server_callback(mod_ble_mesh_config_server_callback);
            esp_ble_mesh_register_prov_callback(mod_ble_mesh_provision_callback);

            if(args[0].u_obj != MP_OBJ_NULL) {
                err = esp_ble_mesh_set_unprovisioned_device_name(mp_obj_str_get_str(args[0].u_obj));
            }
            else {
                err = esp_ble_mesh_set_unprovisioned_device_name(MOD_BLE_MESH_DEFAULT_NAME);
            }

            if(err != ESP_OK) {
                // TODO: drop back the error code
                nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "BLE Mesh node name cannot be set, error code: %d!", err));
            }

            err = esp_ble_mesh_init(provision_ptr, composition_ptr);

            if(err != ESP_OK) {
                // TODO: drop back the error code
                nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_RuntimeError, "BLE Mesh module could not be initialized, error code: %d!", err));
            }
            else {
                initialized = true;
            }
        }
        else {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_MemoryError, "BLE Mesh module could not be initialized!"));
        }
    }
    else {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "BLE Mesh module is already initialized!"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ble_mesh_init_obj, 0, mod_ble_mesh_init);

// Set node provisioning
STATIC mp_obj_t mod_ble_mesh_set_node_prov(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    STATIC const mp_arg_t mod_ble_mesh_set_node_prov_args[] = {
            { MP_QSTR_bearer,                MP_ARG_INT,                   {.u_int = 4}},
            { MP_QSTR_callback,              MP_ARG_OBJ  | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_ble_mesh_set_node_prov_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_ble_mesh_set_node_prov_args, args);

    // Get bearer type
    int type = args[0].u_int;

    // Get MP callback
    provision_callback = args[1].u_obj;

    // Disable node provision in prior
    esp_ble_mesh_node_prov_disable(MOD_BLE_MESH_PROV_ADV|MOD_BLE_MESH_PROV_GATT);

    // Check if provision mode is within valid range
    if((type >= MOD_BLE_MESH_PROV_ADV) && (type <= (MOD_BLE_MESH_PROV_ADV|MOD_BLE_MESH_PROV_GATT))) {
        esp_ble_mesh_node_prov_enable(type);
    }
    else if(type != MOD_BLE_MESH_PROV_NONE) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Node provision mode is not valid!"));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ble_mesh_set_node_prov_obj, 0, mod_ble_mesh_set_node_prov);

// Reset node provisioning information
STATIC mp_obj_t mod_ble_mesh_reset_node_prov(void) {

    // Delete and reset node provision information
    esp_ble_mesh_node_local_reset();

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_ble_mesh_reset_node_prov_obj, mod_ble_mesh_reset_node_prov);

// Set node provisioning
STATIC mp_obj_t mod_ble_mesh_input_oob(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    STATIC const mp_arg_t mod_ble_mesh_input_oob_args[] = {
            { MP_QSTR_oob,                   MP_ARG_OBJ,                   {.u_obj = MP_OBJ_NULL}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_ble_mesh_input_oob_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_ble_mesh_input_oob_args, args);

    if(args[0].u_obj != MP_OBJ_NULL) {
        if(mp_obj_is_int(args[0].u_obj)) {
            esp_ble_mesh_node_input_number(mp_obj_get_int(args[0].u_obj));
        }
        else if(mp_obj_is_str(args[0].u_obj)) {
            esp_ble_mesh_node_input_string(mp_obj_get_type_str(args[0].u_obj));
        }
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ble_mesh_input_oob_obj, 0, mod_ble_mesh_input_oob);

// TODO: add parameters for configuring the Configuration Server Model
STATIC mp_obj_t mod_ble_mesh_create_element(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    STATIC const mp_arg_t mod_ble_mesh_create_element_args[] = {
            { MP_QSTR_primary,               MP_ARG_BOOL | MP_ARG_KW_ONLY | MP_ARG_REQUIRED },
            { MP_QSTR_feature,               MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 0}},
            { MP_QSTR_beacon,                MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true}},
            { MP_QSTR_ttl,                   MP_ARG_INT  | MP_ARG_KW_ONLY, {.u_int = 7}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(mod_ble_mesh_create_element_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(args), mod_ble_mesh_create_element_args, args);

    // Get primary bool
    bool primary = args[0].u_bool;

    if(primary) {
        // TODO: check here if not other primary element exists
        if(1) {
            // Get Configuration Server Model parameters
            int feature = args[1].u_int;
            bool beacon = args[2].u_bool;
            int ttl = args[3].u_int;

            //TODO: add support for several more elements
            mod_ble_mesh_element_class_t *ble_mesh_element = &mod_ble_mesh_element;

            // Initiate an empty element
            ble_mesh_element->base.type = &mod_ble_mesh_element_type;
            ble_mesh_element->element = (mod_ble_mesh_elem_t *)heap_caps_calloc(1, sizeof(mod_ble_mesh_elem_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

            // Add the mandatory Configuration Server Model
            esp_ble_mesh_cfg_srv_t* configuration_server_model_ptr = (esp_ble_mesh_cfg_srv_t *)heap_caps_calloc(1, sizeof(esp_ble_mesh_cfg_srv_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

            // Configure the Configuration Server Model
            configuration_server_model_ptr->relay = ((feature & MOD_BLE_MESH_RELAY) > 0);
            configuration_server_model_ptr->friend_state = ((feature & MOD_BLE_MESH_FRIEND) > 0);
            configuration_server_model_ptr->gatt_proxy = ((feature & MOD_BLE_MESH_GATT_PROXY) > 0);

            configuration_server_model_ptr->beacon = beacon;
            configuration_server_model_ptr->default_ttl = ttl;

            configuration_server_model_ptr->net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20); /* 3 transmissions with 20ms interval */
            configuration_server_model_ptr->relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20);

            // Prepare temp model
            esp_ble_mesh_model_t tmp_model = ESP_BLE_MESH_MODEL_CFG_SRV(configuration_server_model_ptr);

            // Allocate memory for the model
            ble_mesh_element->element->sig_models = (esp_ble_mesh_model_t*)heap_caps_calloc(1, sizeof(esp_ble_mesh_model_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            // Copy the already prepared element to the new location
            memcpy(ble_mesh_element->element->sig_models, &tmp_model, sizeof(esp_ble_mesh_model_t));

            // Create the MicroPython Model
            // TODO: add callback and value
            (void)mod_ble_add_model_to_list(ble_mesh_element, 0, NULL, NULL);
            // This is the first model
            ble_mesh_element->element->sig_model_count = 1;

            return ble_mesh_element;
        }
        else {
            // TODO: add support for more elements
            nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Only one primary element is allowed!"));

            return mp_const_none;
        }
    }
    else {
        // Can be added here empty secondary element
        //TODO: add support for more elements
        nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "Only primary element is supported now!"));
        //Return none until not implemented scenario
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ble_mesh_create_element_obj, 0, mod_ble_mesh_create_element);


STATIC const mp_map_elem_t mod_ble_mesh_globals_table[] = {
        { MP_OBJ_NEW_QSTR(MP_QSTR___name__),                       MP_OBJ_NEW_QSTR(MP_QSTR_BLE_Mesh) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_init),                           (mp_obj_t)&mod_ble_mesh_init_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_set_node_prov),                  (mp_obj_t)&mod_ble_mesh_set_node_prov_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_reset_node_prov),                (mp_obj_t)&mod_ble_mesh_reset_node_prov_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_input_oob),                      (mp_obj_t)&mod_ble_mesh_input_oob_obj },
        { MP_OBJ_NEW_QSTR(MP_QSTR_create_element),                 (mp_obj_t)&mod_ble_mesh_create_element_obj },

        // Constants of Advertisement
        { MP_OBJ_NEW_QSTR(MP_QSTR_PROV_ADV),                       MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_PROV_ADV) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_PROV_GATT),                      MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_PROV_GATT) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_PROV_NONE),                      MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_PROV_NONE) },
        // Constants of Node Features
        { MP_OBJ_NEW_QSTR(MP_QSTR_RELAY),                          MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_RELAY) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_LOW_POWER),                      MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_LOW_POWER) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_GATT_PROXY),                     MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_GATT_PROXY) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_FRIEND),                         MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_FRIEND) },
        // Constants of Authentication
        { MP_OBJ_NEW_QSTR(MP_QSTR_OOB_INPUT),                      MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_INPUT_OOB) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_OOB_OUTPUT),                     MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_OUTPUT_OOB) },

        // Constants of Server-Client
        { MP_OBJ_NEW_QSTR(MP_QSTR_SERVER),                         MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_SERVER) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_CLIENT),                         MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_CLIENT) },
        // Constants of Model types
        { MP_OBJ_NEW_QSTR(MP_QSTR_GENERIC),                        MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_MODEL_GENERIC) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_SENSORS),                        MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_MODEL_SENSORS) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_TIME_SCENES),                    MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_MODEL_TIME_SCENES) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_LIGHTNING),                      MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_MODEL_LIGHTNING) },
        // Constants of functionality behind a model
        { MP_OBJ_NEW_QSTR(MP_QSTR_ONOFF),                          MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_MODEL_ONOFF) },
        { MP_OBJ_NEW_QSTR(MP_QSTR_LEVEL),                          MP_OBJ_NEW_SMALL_INT(MOD_BLE_MESH_MODEL_LEVEL) },

};

STATIC MP_DEFINE_CONST_DICT(mod_ble_mesh_globals, mod_ble_mesh_globals_table);

// Sub-module of Bluetooth module (modbt.c)
const mp_obj_module_t mod_ble_mesh = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mod_ble_mesh_globals,
};
