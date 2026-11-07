#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/twai.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include <atomic>

#define I2C_MASTER_SCL_IO      GPIO_NUM_4
#define I2C_MASTER_SDA_IO      GPIO_NUM_5
#define I2C_MASTER_NUM     I2C_NUM_0    

#define ES_GPIO GPIO_NUM_12
#define DS_GPIO GPIO_NUM_9
#define TS_GPIO GPIO_NUM_13

// Configurações dos pinos do CAN e IDs
#define TX_GPIO_NUM       GPIO_NUM_19
#define RX_GPIO_NUM       GPIO_NUM_20
#define NODE_ID           0x0bf  // Mude este ID para cada ESP periférico (ex: 0x102, 0x103...)
#define SEND_DELAY_MS     5000  // Envia a cada 5 segundos

// Prioridade de Tasks
#define TX_TASK_PRIO      8
#define SENSOR_TASK_PRIO  9

#define I2C_SHT41_ADD 0x44
#define V_REF 1100

typedef struct {
    int temperatura;
    int rh;
} sensor_data_t;

QueueHandle_t can_queue;

const char* TAG = "TEST";

static const twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
static const twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
static const twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO_NUM, RX_GPIO_NUM, TWAI_MODE_NORMAL);

static void sensor_aquire(void *arg)
{

   sensor_data_t dados_adquiridos;

   i2c_master_bus_config_t i2c_mst_config = {};
   i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
   i2c_mst_config.i2c_port = I2C_MASTER_NUM;
   i2c_mst_config.scl_io_num = I2C_MASTER_SCL_IO;
   i2c_mst_config.sda_io_num = I2C_MASTER_SDA_IO;
   i2c_mst_config.glitch_ignore_cnt = 7;
   i2c_mst_config.flags.enable_internal_pullup = true;
   

   i2c_master_bus_handle_t bus_handle;
   ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));

   i2c_device_config_t dev_cfg = {};
   dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
   dev_cfg.device_address = I2C_SHT41_ADD;
   dev_cfg.scl_speed_hz = 100000;

   i2c_master_dev_handle_t dev_handle;
   ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));

   // Configuração do ADC

   adc_unit_t u;
   adc_channel_t adc_channel;

   adc_oneshot_io_to_channel(TS_GPIO, &u , &adc_channel);

   adc_oneshot_unit_init_cfg_t config_adc = {};
   config_adc.unit_id = u;
   config_adc.ulp_mode = ADC_ULP_MODE_DISABLE;
   config_adc.clk_src = ADC_RTC_CLK_SRC_DEFAULT;


   adc_oneshot_unit_handle_t adc_handle;

   adc_oneshot_new_unit(&config_adc, &adc_handle);


   adc_oneshot_chan_cfg_t config_channel;
   config_channel.atten = ADC_ATTEN_DB_12;
   config_channel.bitwidth = ADC_BITWIDTH_12;

   adc_oneshot_config_channel(adc_handle, adc_channel, &config_channel);
   //

   ESP_LOGI(TAG, "Comecando Monitoramento...");

    while (1) 
   {
      uint8_t read_buf[6] = {};

      uint8_t written = 0xFD;

      i2c_master_transmit
      (
         dev_handle,
         &written,
         1,
         -1
      );

      //ESP_LOGI(TAG, "Request mandado");

      vTaskDelay(pdMS_TO_TICKS(10));

      i2c_master_receive
      (
         dev_handle,
         read_buf,
         6,
         -1
      );

      //ESP_LOGI(TAG, "Transacao finalizada");

      ESP_LOGI(TAG, "--------Comeco do LOG---------");

      uint32_t tick_temp = read_buf[0]*256 + read_buf[1];

      uint32_t tick_rh = read_buf[3]*256 + read_buf[4];


      float temp= -45 + 175*((float)tick_temp/65535);

      float rh = -6 + 125*((float)tick_rh/65535);

      //ESP_LOGI(TAG, "Temperatura tick: %d" , tick_temp);

      //ESP_LOGI(TAG, "Umidade tick: %d" , tick_rh);


      if(rh > 100)
      {
         rh = 100;
      }
      else if(rh < 0)
      {
         rh = 0;
      }

      int rh_ar = (int) rh;
      int temp_ar = (int) temp;

      ESP_LOGI(TAG, "Temperatura: %d" ,  temp_ar);

      ESP_LOGI(TAG, "Umidade: %d" ,  rh_ar);
   
      if(!gpio_get_level(ES_GPIO))
      {
         ESP_LOGI(TAG, "Conectado à rede elétrica");
      }
      else
      {
         ESP_LOGI(TAG, "Desconectado da rede elétrica");
      }

      if(!gpio_get_level(DS_GPIO))
      {
         ESP_LOGI(TAG, "Porta Aberta");
      }
      else
      {
         ESP_LOGI(TAG, "Porta Fechada");
      }

      int V_out;

      adc_oneshot_read(adc_handle , adc_channel , &V_out);

      float V_data = (float(V_out)/4095)*V_REF*3.41f;

      float temp_analogico = (V_data-300.f)/30.f;


      //ESP_LOGI(TAG, "tensao Analogico: %f" ,  V_data);
      //ESP_LOGI(TAG, "Temperatura Analogico: %f" ,  temp_analogico);

      dados_adquiridos.temperatura = temp_ar;
      dados_adquiridos.rh = rh_ar;

      ESP_LOGI(TAG, "--------Final do LOG---------");

      if (xQueueSend(can_queue, &dados_adquiridos, 0) != pdTRUE) {
                    ESP_LOGW("SENSOR_AQUIRE_TASK", "Fila cheia! Dado ignorado.");
                }

      vTaskDelay(pdMS_TO_TICKS(SEND_DELAY_MS-10));

   };
}



static void twai_transmit_task(void *arg)
{
    ESP_LOGI(TAG, "Iniciando envio de mensagens...");

    sensor_data_t dados_recebidos;

    while (1) {
        // Criação da mensagem de dados
        twai_message_t tx_msg = {
            
            .extd = 0,              // ID Padrão (11 bits)
            .rtr = 0,               // Frame de dados
            .ss = 0,                // Tenta retransmitir se houver erro
            .self = 0,
            .dlc_non_comp = 0,
            .identifier = NODE_ID,  // ID único deste ESP
            .data_length_code = 2,  // Enviando 4 bytes
            .data = {10, 20} // Seus dados (Temperatura, status, etc)
        };

        if (xQueueReceive(can_queue, &dados_recebidos, portMAX_DELAY) == pdTRUE) {
            

            tx_msg.data[0] = dados_recebidos.rh;
            tx_msg.data[1] = dados_recebidos.temperatura;

            ESP_LOGI("TWAI_TASK", "Dado processado pela fila e enviado para o Barramento");
        }

        esp_err_t result = twai_transmit(&tx_msg, pdMS_TO_TICKS(1000));

        // Envia a mensagem para o barramento
        
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "Mensagem enviada com sucesso pelo Node 0x%x", NODE_ID);
        } else {
            ESP_LOGE(TAG, "Falha ao enviar mensagem: %s", esp_err_to_name(result));
        }

        // Aguarda o tempo definido antes de enviar a próxima
        vTaskDelay(pdMS_TO_TICKS(SEND_DELAY_MS));
    }
}



extern "C" void app_main(void)
{

   can_queue = xQueueCreate(10, sizeof(sensor_data_t));
    if (can_queue == NULL) {
        ESP_LOGE("MAIN", "Falha ao criar a fila!");
        return;
    }

   gpio_config_t config = {};
   config.pin_bit_mask = (1ULL << ES_GPIO);
   config.mode = GPIO_MODE_INPUT;
   config.pull_down_en = GPIO_PULLDOWN_DISABLE;
   config.pull_up_en = GPIO_PULLUP_DISABLE;
   config.intr_type = GPIO_INTR_DISABLE;

   gpio_config(&config);

   config = {};
   config.pin_bit_mask = (1ULL << DS_GPIO);
   config.mode = GPIO_MODE_INPUT;
   config.pull_down_en = GPIO_PULLDOWN_DISABLE;
   config.pull_up_en = GPIO_PULLUP_DISABLE;
   config.intr_type = GPIO_INTR_DISABLE;

   gpio_config(&config);

   // Configuração do CAN (TWAI)
   ESP_LOGI(TAG, "Instalando driver TWAI...");
   ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
   ESP_LOGI(TAG, "Iniciando driver TWAI...");
   ESP_ERROR_CHECK(twai_start());
   xTaskCreatePinnedToCore(twai_transmit_task, "TWAI_tx", 4096, NULL, TX_TASK_PRIO, NULL, tskNO_AFFINITY);
   xTaskCreatePinnedToCore(sensor_aquire, "Sensor_data", 4096, NULL, SENSOR_TASK_PRIO, NULL, tskNO_AFFINITY);
   //
}