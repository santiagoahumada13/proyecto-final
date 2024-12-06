//FreeRTOS includes
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "semphr.h"
//Standard includes
#include <stdio.h>
#include <stdint.h>
//Pico includes
#include <pico/stdlib.h>
#include "pico/binary_info.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
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
static QueueHandle_t xQueueterminalTemp= NULL;
static QueueHandle_t xQueueterminalPress= NULL;

const uint8_t num_chars_per_disp[]={7,7,7,5};

const char *ChannelID="2769474";
#define MQTT_SERVER_HOST "mqtt3.thingspeak.com"
#define MQTT_SERVER_PORT 1883
#define MQTT_CLIENT_ID "CDY0IjMeGCgiNz0aOiMQDSU"
#define MQTT_USERNAME "CDY0IjMeGCgiNz0aOiMQDSU"
#define MQTT_PASSWORD "tQM4LoDla8sePSQudlIe/wkl"


typedef struct MQTT_CLIENT_T_ {
  ip_addr_t remote_addr;
  mqtt_client_t *mqtt_client;
  u32_t received;
  u32_t counter;
  u32_t reconnect;
} MQTT_CLIENT_T;

static MQTT_CLIENT_T* mqtt_client_init(void) {
  MQTT_CLIENT_T *stateM = (MQTT_CLIENT_T *)calloc(1, sizeof(MQTT_CLIENT_T));    
  if (!stateM) {
    printf("-failed to allocate state\n");
    return NULL;
  }
  stateM->received = 0;
  return stateM;
}

MQTT_CLIENT_T *stateM;
SemaphoreHandle_t mqttSemaphore;

void dns_found(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
    MQTT_CLIENT_T *stateM = (MQTT_CLIENT_T *)callback_arg;
    printf("-MQTT DNS addr: %s.\n", ip4addr_ntoa(ipaddr));
    stateM->remote_addr = *ipaddr;
    xSemaphoreGive(mqttSemaphore);  // Liberar semáforo
}

void run_dns_lookup(void *pvParameters) {
    printf("\n-Running mqtt DNS query for %s.\n", MQTT_SERVER_HOST);

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(MQTT_SERVER_HOST, &(stateM->remote_addr), dns_found, stateM);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        printf("-No DNS lookup needed.\n");
        xSemaphoreGive(mqttSemaphore);  // Liberar semáforo si ya está resuelto
    } else if (err == ERR_ARG) {
        printf("-Failed to start MQTT DNS query\n");
    }

    // Liberar el control de la tarea
    vTaskDelete(NULL);
}

void mqtt_pub_request_cb(void *arg, err_t err) {
    if (err != ERR_OK) {
        printf("-Publish failed with error: %d\n", err);
    } else {
        printf("-Publish successful.\n");
    }
}
void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    MQTT_CLIENT_T *stateM = (MQTT_CLIENT_T *)arg;

    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("MQTT connected successfully.\n");
    } else {
        printf("MQTT connection failed with status: %d\n", status);
        stateM->reconnect++;
    }
}
/*En FreeRTOS se utilizan "tareas" que son rutinas declaradas como void que no 
retornan ningun valor. Cada una de estas rutinas envia o recibe datos a traves
de una cola o "Queue". 
*/


void mqtt_task(void *pvParameters) {
    static const struct mqtt_connect_client_info_t mqtt_client_info = {
        .client_id = MQTT_CLIENT_ID,
        .client_user = MQTT_USERNAME,
        .client_pass = MQTT_PASSWORD,
        .keep_alive = 60,
        .will_topic = NULL,
    };

    if (!stateM->mqtt_client) {
        stateM->mqtt_client = mqtt_client_new();
        if (!stateM->mqtt_client) {
            printf("Failed to create MQTT client.\n");
            return;
        }
    }

    while (1) {
        int32_t receivedTemp;
        int32_t receivedPress;

        xQueueReceive(xQueueTemp, &receivedTemp, portMAX_DELAY);
        float temperature = receivedTemp / 100.f;

        xQueueReceive(xQueuePress, &receivedPress, portMAX_DELAY);
        float pressure = receivedPress / 1000.f;

        if (!mqtt_client_is_connected(stateM->mqtt_client)) {
            err_t err = mqtt_client_connect(
                stateM->mqtt_client,
                &stateM->remote_addr,
                MQTT_SERVER_PORT,
                mqtt_connection_cb,
                stateM,
                &mqtt_client_info
            );

            if (err != ERR_OK) {
                printf("MQTT connection failed: %d\n", err);
                continue;
            }
        }

        char temp_buffer[64];
        snprintf(temp_buffer, sizeof(temp_buffer), "field1=%.2f&api_key=%s", temperature, "3UAEI0AXFKA2PL3O");
        err_t err = mqtt_publish(
            stateM->mqtt_client,
            "channels/2769474/publish",
            temp_buffer,
            strlen(temp_buffer),
            0,
            0,
            mqtt_pub_request_cb,
            stateM
        );

        if (err != ERR_OK) {
            printf("Failed to publish temperature: %d\n", err);
        }

        char pressure_buffer[64];
        snprintf(pressure_buffer, sizeof(pressure_buffer), "field2=%.2f&api_key=%s", pressure, "3UAEI0AXFKA2PL3O");
        err = mqtt_publish(
            stateM->mqtt_client,
            "channels/2769474/publish",
            pressure_buffer,
            strlen(pressure_buffer),
            0,
            0,
            mqtt_pub_request_cb,
            stateM
        );

        if (err != ERR_OK) {
            printf("Failed to publish pressure: %d\n", err);
        }

        vTaskDelay(pdMS_TO_TICKS(15000)); // Publish every 15 seconds
    }
}

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
        xQueueSend(xQueueTemp, &temperature, portMAX_DELAY);// Funcion para enviar datos en una cola hacia las funciones mqtt_send y oled_task
        xQueueSend(xQueueterminalTemp, &temperature, portMAX_DELAY);// Funcion para enviar datos en una cola hacia la funcion usb_task
        int32_t pressure = bmp280_convert_pressure(raw_pressure, raw_temperature, &params);
        xQueueSend(xQueuePress, &pressure, portMAX_DELAY);
        xQueueSend(xQueueterminalPress, &pressure, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
//Tarea para enviar los datos mediante USB y mostrarlos en un monitor serial
void usb_task(void *pvParameters){
    int32_t receivedTemp;
    int32_t receivedPress;

    while(1){
        xQueueReceive(xQueueterminalTemp, &receivedTemp, portMAX_DELAY);
        printf("Temperatura = %.2f C\n", receivedTemp/100.f);
        xQueueReceive(xQueueterminalPress, &receivedPress, portMAX_DELAY); //Funcion para recibir datos de la cola
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
        stateM = mqtt_client_init();
        if (!stateM) {
            printf("-Failed to initialize MQTT client\n");
            return -1;
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

        mqttSemaphore = xSemaphoreCreateBinary();
        xQueueTemp = xQueueCreate(2,sizeof(int32_t));
        xQueuePress = xQueueCreate(2,sizeof(int32_t));
        xQueueterminalTemp = xQueueCreate(2,sizeof(int32_t));
        xQueueterminalPress = xQueueCreate(2,sizeof(int32_t));
        xTaskCreate(run_dns_lookup, "dnslookup_Task", 1024, NULL, 1, NULL);
        xTaskCreate(mqtt_task, "mqtt_Task", 2048, NULL, 1, NULL);
        xTaskCreate(tempBMP_task, "tempBMP_Task",256, NULL, 1, NULL);
        xTaskCreate(oled_task, "toled_Task" , 256 , NULL, 1 , NULL);
        xTaskCreate(usb_task, "usb_Task" , 256 , NULL , 1 , NULL);
        vTaskStartScheduler();

        while(1){}
    #endif
}