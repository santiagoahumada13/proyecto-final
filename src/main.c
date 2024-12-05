//FreeRTOS includes
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
//Standard includes
#include <stdio.h>
#include <stdint.h>
//Pico includes
#include <pico/stdlib.h>
#include "pico/binary_info.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcpip.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
//Peripherals includes
#include "bmp280_i2c.h"
#include "ssd1306.h"
//MQTT lwip app
#include "lwip/apps/mqtt.h"

//Wifi Config

//Las colas se determinan segun los datos a enviar (o al menos eso entendi)

static QueueHandle_t xQueueTemp= NULL;
static QueueHandle_t xQueuePress= NULL;

const uint8_t num_chars_per_disp[]={7,7,7,5};

const char *ChannelID= "2769474";
#define MQTT_BROKER_IP "184.106.153.149"
#define MQTT_BROKER_PORT 1883
#define TS_API_KEY "3UAEI0AXFKA2PL3O"
#define TOPIC_TEMPERATURE "channels/2769474/publish/fields/1"
#define TOPIC_PRESSURE "channels/2769474/publish/fields/2"

static void publish_to_topic(mqtt_client_t *client, const char *topic, float value) {
    char payload[64];
    snprintf(payload, sizeof(payload), "%.2f", value);

    err_t err = mqtt_publish(client, topic, payload, strlen(payload), 0, 0, NULL, NULL);
    if (err == ERR_OK) {
        printf("Dato enviado al tópico %s: %s\n", topic, payload);
    } else {
        printf("Error enviando dato al tópico %s: %d\n", topic, err);
    }
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("Conectado a ThingSpeak MQTT\n");
    } else {
        printf("Error de conexión MQTT: %d\n", status);
    }
}

void mqttsend_task() {
    mqtt_client_t *client = mqtt_client_new();
    if (client == NULL) {
        printf("Failed to create MQTT client\n");
        return;
    }
    struct mqtt_connect_client_info_t ci = {
        .client_id = "DzcaDRwSPSgBOQ4YOTomNRE",
        .client_user = NULL,
        .client_pass = NULL,
        .keep_alive = 60,
    };

    ip_addr_t broker_ip;
    ipaddr_aton(MQTT_BROKER_IP, &broker_ip);

    err_t err = mqtt_client_connect(client, &broker_ip, MQTT_BROKER_PORT, mqtt_connection_cb, NULL, &ci);
    if (err != ERR_OK) {
        printf("Error connecting to MQTT broker: %d\n", err);
        return;
    }
        
    while (true) {
        // Lee los datos del BMP280
        int32_t receivedTemp;
        int32_t receivedPress;
        xQueueReceive(xQueueTemp, &receivedTemp, portMAX_DELAY);
        xQueueReceive(xQueuePress, &receivedPress, portMAX_DELAY);
        float temp=receivedTemp/100.f;
        float press=receivedPress/1000.f;
    
        // Publica los datos en los tópicos correspondientes
        publish_to_topic(client, TOPIC_TEMPERATURE, temp);
        publish_to_topic(client, TOPIC_PRESSURE, press);

        vTaskDelay(pdMS_TO_TICKS(15000)); // ThingSpeak permite una publicación cada 15 segundos
    }
}
/*En FreeRTOS se utilizan "tareas" que son rutinas declaradas como void que no 
retornan ningun valor. Cada una de estas rutinas envia o recibe datos a traves
de una cola o "Queue". 
*/

//Tarea para obtener los valores de temperatura y presion del sensor BMP280

void tempBMP_task(void *pvParameters){
    bmp280_init();
    struct bmp280_calib_param params;
    bmp280_get_calib_params(&params);
    int32_t raw_temperature;
    int32_t raw_pressure;
    vTaskDelay(pdMS_TO_TICKS(200));
    while (1) {
        bmp280_read_raw(&raw_temperature, &raw_pressure);
        int32_t temperature = bmp280_convert_temp(raw_temperature, &params);
        xQueueSend(xQueueTemp, &temperature, portMAX_DELAY);// Funcion para enviar datos en una cola
        int32_t pressure = bmp280_convert_pressure(raw_pressure, raw_temperature, &params);
        xQueueSend(xQueuePress, &pressure, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(7500));
    }
}
//Tarea para enviar los datos mediante USB y mostrarlos en un monitor serial
void usb_task(void *pvParameters){
    int32_t receivedTemp;
    int32_t receivedPress;

    while(1){
        xQueueReceive(xQueueTemp, &receivedTemp, portMAX_DELAY);
        printf("Temperatura = %.2f C\n", receivedTemp/100.f);
        xQueueReceive(xQueuePress, &receivedPress, portMAX_DELAY); //Funcion para recibir datos de la cola
        printf("Presion = %.3f kPa\n", receivedPress/1000.f);
    }
}
//Tarea par mostrar los datos en la pantalla OLED 128x64
void oled_task(void *pvParameters){

    int32_t receivedTemp;
    int32_t receivedPress;
    
    while(1){
        xQueueReceive(xQueueTemp, &receivedTemp, portMAX_DELAY);
        float temp=receivedTemp/100.f;
        xQueueReceive(xQueuePress, &receivedPress, portMAX_DELAY);
        float press=receivedPress/1000.f;
        char tcon[20];
        char pcon[20];
        sprintf(tcon,"%.0f C",temp);
        sprintf(pcon,"%.0fkPa",press);
        char *meas[]={tcon,pcon};
        ssd1306_t disp;
        disp.external_vcc=false;
        ssd1306_init(&disp, 128, 64, 0x3C, i2c0);
        ssd1306_clear(&disp);
        const char *words[]={"Temperatura","Presion"};
        char buf[8];

        for(;;){
                for(int i=0; i<sizeof(meas)/sizeof(char *); ++i) {
                    ssd1306_draw_string(&disp, 8, 8, 3/2, words[i]);
                    ssd1306_draw_string(&disp, 8, 32, 4, meas[i]);
                    ssd1306_show(&disp);
                    ssd1306_clear(&disp);
                    sleep_ms(1000);
                    
                }
            }
    }
}
//Main
/*Al utilizar FreeRTOS el main tiene la funcion principal de: inicializar puertos 
y crear las tareas para "agendar" su ejecuciones, mediante las funciones xTaskCreate
y vTaskScheduler, ademas de tambien crear las colas o "Queues"
*/
int main(){
    stdio_init_all();
    #if !defined(i2c_default) || !defined(PICO_DEFAULT_I2C_SDA_PIN) || !defined(PICO_DEFAULT_I2C_SCL_PIN)
    #warning i2c / bmp280_i2c example requires a board with I2C pins
        puts("Default I2C pins were not defined");
    return 0;
    #else
        bi_decl(bi_2pins_with_func(PICO_DEFAULT_I2C_SDA_PIN, PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C));
        bi_decl(bi_program_description("BMP280 I2C example for the Raspberry Pi Pico"));
        if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

        cyw43_arch_enable_sta_mode();

        printf("Connecting to Wi-Fi...\n");
        if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
            printf("failed to connect.\n");
            return 1;
        } else {
            printf("Connected.\n");
        }
        i2c_init(i2c_default, 400 * 1000);
        gpio_set_function(17, GPIO_FUNC_I2C);
        gpio_set_function(16, GPIO_FUNC_I2C);
        gpio_pull_up(17);
        gpio_pull_up(16);

        xQueueTemp = xQueueCreate(2,sizeof(int32_t));
        xQueuePress = xQueueCreate(2,sizeof(int32_t));
        //xTaskCreate(mqttconnect_task, "mqttconnect_task",NULL,1,NULL);
        xTaskCreate(tempBMP_task, "tempBMP_Task",256, NULL, 1, NULL);
        xTaskCreate(oled_task, "toled_Task" , 256 , NULL, 1 , NULL);
        xTaskCreate(usb_task, "usb_Task" , 256 , NULL , 1 , NULL);
        xTaskCreate(mqttsend_task, "mqttsend_Task" , 256 , NULL , 1 , NULL);
        vTaskStartScheduler();

        while(1){}
    #endif
}