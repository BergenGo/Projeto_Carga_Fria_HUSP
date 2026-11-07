#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/twai.h"
#include "mqtt_client.h"

#define WIFI_SSID      "Bergeno"
#define WIFI_PASS      "Thx1138@"
#define TAG_WIFI       "WIFI_STATION"
#define TAG "THINGSBOARD_MQTT"

#define THINGSBOARD_URI   "mqtt://thingsboard.cloud" 
#define THINGSBOARD_TOKEN "z4s63x1pfl1sanbjjxe7"    // Copie o Access Token do dispositivo no ThingsBoard

#define MQTT_TASK_PRIO    8
#define RX_TASK_PRIO    9
#define TX_GPIO_NUM     17
#define RX_GPIO_NUM     16
#define EXAMPLE_TAG     "TWAI_RX"

static esp_mqtt_client_handle_t mqtt_client = NULL;

// Estrutura da fila
typedef struct {
    float temperatura;
    int rph;
} sensor_data_t;

// Handle global da fila
QueueHandle_t mqtt_queue;

// Aceita qualquer ID de mensagem
static const twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

// Configuração de velocidade (100kbps - altere se sua rede usar outra velocidade)
static const twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();

// Configuração geral
static const twai_general_config_t g_config = {
    .mode = TWAI_MODE_NORMAL, // MODO NORMAL: O hardware do ESP envia o ACK automaticamente!
    .tx_io = TX_GPIO_NUM, 
    .rx_io = RX_GPIO_NUM,
    .clkout_io = TWAI_IO_UNUSED, 
    .bus_off_io = TWAI_IO_UNUSED,
    .tx_queue_len = 0, // 0 porque não vamos transmitir mensagens por software
    .rx_queue_len = 5, // Fila para armazenar até 5 mensagens recebidas
    .alerts_enabled = TWAI_ALERT_NONE,
    .clkout_divider = 0
};

// Callback para gerenciar os eventos de conexão MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado com sucesso ao ThingsBoard!");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Desconectado do ThingsBoard.");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Dado publicado com sucesso! MSG_ID=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Erro no MQTT!");
            break;
        default:
            break;
    }
}

// Inicializa o cliente MQTT
void start_mqtt_thingsboard(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = THINGSBOARD_URI,
        .credentials.username = THINGSBOARD_TOKEN, // O token do device vai no campo Username
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Função para enviar os dados que você adquiriu (ex: dados do TWAI/Sensores)
void send_telemetry_to_thingsboard(float temperatura, int rph)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Cliente MQTT não inicializado.");
        return;
    }

    // 1. O ThingsBoard exige que os dados sejam enviados no formato JSON.
    char payload_json[128];
    snprintf(payload_json, sizeof(payload_json), "{\"temperature\": %.2f, \"humidity\": %d}", temperatura, rph);

    // 2. O tópico para telemetria no ThingsBoard é SEMPRE "v1/devices/me/telemetry"
    // Parâmetros: (cliente, tópico, payload, tamanho, qos, retain)
    int msg_id = esp_mqtt_client_publish(mqtt_client, "v1/devices/me/telemetry", payload_json, 0, 1, 0);
    
    ESP_LOGI(TAG, "Enviando: %s", payload_json);
}


// Tratador de eventos do Wi-Fi
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG_WIFI, "Tentando conectar ao Wi-Fi...");
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG_WIFI, "Desconectado. Tentando reconectar...");
        esp_wifi_connect(); // Tenta reconectar automaticamente
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "Conectado! Endereço IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // --- AQUI VOCÊ INICIA O MQTT ---
        start_mqtt_thingsboard(); 
    }
}

void wifi_init_sta(void)
{
    // 1. Inicializa a interface de rede subjacente
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 2. Configurações padrão do Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3. Registra o tratador de eventos para eventos de Wi-Fi e de IP
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    // 4. Configura as credenciais da rede
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // Segurança padrão da maioria dos roteadores
        },
    };

    // 5. Aplica configurações e inicia
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WIFI, "Inicialização do Wi-Fi concluída.");
}

// Task para escutar a Rede
static void twai_receive_task(void *arg)
{
    ESP_LOGI(EXAMPLE_TAG, "Iniciando escuta da rede CAN...");
    
    while (1) {
        twai_message_t rx_msg;
        
        
        if (twai_receive(&rx_msg, portMAX_DELAY) == ESP_OK) 
        {
            ESP_LOGI(EXAMPLE_TAG, "Mensagem Recebida! ID: 0x%03lX | DLC: %d", 
                     rx_msg.identifier, rx_msg.data_length_code);

            if (rx_msg.identifier == 0x0BF) {
                sensor_data_t dados_coletados;
                
                // Extrai os dados do payload da mensagem CAN (exemplo fictício)
                dados_coletados.rph = rx_msg.data[0]; 
                dados_coletados.temperatura = rx_msg.data[1];

                // Envia a struct para a fila. 
                // Usamos timeout 0 (não bloqueante) para não travar a leitura do CAN 
                // caso o MQTT demore (ex: internet lenta) e a fila encha.
                if (xQueueSend(mqtt_queue, &dados_coletados, 0) != pdTRUE) {
                    ESP_LOGW("TWAI_TASK", "Fila cheia! Dado ignorado.");
                }
            }
        }
    }
}

static void MQTT_send_task (void *arg) {
    sensor_data_t dados_recebidos;

    ESP_LOGI("MQTT_TASK", "Task de publicacao iniciada.");

    while (1) {
        // Fica travada aqui até chegar um dado na fila (portMAX_DELAY)
        if (xQueueReceive(mqtt_queue, &dados_recebidos, portMAX_DELAY) == pdTRUE) {
            
            send_telemetry_to_thingsboard(dados_recebidos.temperatura, dados_recebidos.rph);
            
            ESP_LOGI("MQTT_TASK", "Dado processado pela fila e enviado pro MQTT!");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    mqtt_queue = xQueueCreate(10, sizeof(sensor_data_t));
    if (mqtt_queue == NULL) {
        ESP_LOGE("MAIN", "Falha ao criar a fila!");
        return;
    }
    
    // Inicializa o NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Inicia o Wi-Fi
    wifi_init_sta();
    
    // Twai Driver
    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_LOGI(EXAMPLE_TAG, "Driver Instalado");

    // TWAI
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(EXAMPLE_TAG, "Driver Iniciado");

    // Task de Recepção de Dados
    xTaskCreatePinnedToCore(twai_receive_task, "TWAI_rx", 4096, NULL, RX_TASK_PRIO, NULL, tskNO_AFFINITY);
    // Task de Envio ao MQTT
    xTaskCreatePinnedToCore(MQTT_send_task, "MQTT_send", 4096, NULL, MQTT_TASK_PRIO, NULL, 0);

}