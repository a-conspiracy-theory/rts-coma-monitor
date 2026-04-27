/*
    APPLICATION 6
    real-time systems
    Ryan Witt

    Coma Patient Monitoring Service

    AI use - documented in code

    TODO
        Hardware
D           System Heartbeat - LED
D            "Critical Condition" - LED

D            Patient heartbeat monitor - Varistor sensor

D            Patient emergency button - momentary input - toggle critical state
        4 tasks
D            Patient Heartbeat Monitor (implemented with a varistor)
D            Patient Temperature Sensor (implemented with a modified random walker)
D            Dissolved Oxygen Sensor (implemented with a modified random walker)
D            Event Handler Task (this task is variable)

        1 ISR
D            Patient Emergency Button

        Timing
            event handler : hard : 50ms
            patient temperature sensor : soft : 200ms
            patient heartbeat monitor : soft : 100ms
            dissolved oxygen sensor : soft : 100ms
        
        Sync
D            Mutex - LED state access - required because different tasks at different priorities read and write to this variable
D            Binary Sem - Button ISR semaphore - required because doing anything but calling a semaphore in an ISR is bad

        Inter-Task, external Comms
D            Wifi - patient monitoring portal, button for critical state toggle
D            "HR Sensor Data" Queue - hr sense task pushes data to a queue, event handler task transforms it into a data buffer for webpage.
D            "critical condition" event group. hr, tmp, oxy bits. if any are outside of normal ranges, respective task sets the bit.

        Determinism Proof
            "event handler" task duration readout - "system overload" state (?)
D            System Heartbeat LED

        Company Context
            As described

*/


#include <stdio.h>
#include <stdlib.h> 
#include <esp_random.h> //rand for walkers
#include <string.h> //Required by memset
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <esp_http_server.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "math.h"
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/api.h>
#include <lwip/netdb.h>

static const char *TAG = "RTSDEMO"; // TAG for debug

#define LED_RED_PIN GPIO_NUM_4  //LED 1
#define LED_GREEN_PIN GPIO_NUM_5 //LED 2
#define TDR_PIN GPIO_NUM_9 //SENSOR
#define BTN_PIN GPIO_NUM_18 //MOMENTARY INPUT
#define TDR_ADC_CHANNEL ADC1_CHANNEL_8

#define DEBOUNCE_TIME 200

#define MAX_COUNT_SEM 10 

#define BUFFER_SIZE 10

#define HR_THRESH_HI 120
#define HR_THRESH_LO 60
#define TMP_THRESH_HI 40
#define TMP_THRESH_LO 30
#define OX_THRESH_LO 8

#define BIT_HR BIT0
#define BIT_TMP BIT1
#define BIT_OX BIT2

#define HEART_SENSOR_PERIOD 200

#define TMP_OX_SENSOR_PERIOD 100

static volatile TickType_t last_press_t = 0;

bool led_state = 0;

unsigned int critical_states = 0; //bitwise addressed int. 0 in a spot means no critical state. 1 means critical state. cleared on button press.

char sensor_data_string[1024] = "";

//sync vars
    SemaphoreHandle_t led_mutex; //
    SemaphoreHandle_t critical_mutex; //
    SemaphoreHandle_t data_string_mutex; //
    SemaphoreHandle_t springy_mutex;
    SemaphoreHandle_t xButtonSem;
    SemaphoreHandle_t sem_web_on; //
    SemaphoreHandle_t sem_web_off; //

//event group
    EventGroupHandle_t critical_condition;
    EventBits_t critical_bits;

//queue
    QueueHandle_t sensor_queue;

/*-----------------------------------------------------------WIFI-----------------------------------------------------------*/

//                                                           vars
#define EXAMPLE_ESP_WIFI_SSID ""
#define EXAMPLE_ESP_WIFI_PASS ""
#define EXAMPLE_ESP_MAXIMUM_RETRY 5

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5
#define HTTPD_MAX_REQ_HDR_LEN 1024

char html[2048];

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

//                                                          functions
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI("WIFI_EVENT_HANDLER", "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI("WIFI_EVENT_HANDLER", "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI_EVENT_HANDLER", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void connect_wifi(void) 
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    //hook event_handler into the wifi task event caller
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
           // .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("CONNECT_WIFI", "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI("CONNECT_WIFI", "connected to ap: \n\tSSID: %s \n\tpassword: %s\n",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI("CONNECT_WIFI", "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE("CONNECT_WIFI", "UNEXPECTED EVENT");
    }
    //vEventGroupDelete(s_wifi_event_group);
}

esp_err_t send_web_page(httpd_req_t *req) //THIS WAS MODIFIED WITH THE HELP OF CHATGPT / GEMINI. I DO NOT KNOW HTML.
{
    int critical_tmp = 0;
    if(xSemaphoreTake(critical_mutex,5))
    {
        critical_tmp = critical_states;
        xSemaphoreGive(critical_mutex);
    }

    //the new response uses conditional chars in the HTML string. allows for dynamic state stuff. sensor data, warning state, etc.
    const char *bg_color = "#000000";
    const char *warn_text = "NULL";
    char warn_states[50] = "";
    if(xSemaphoreTake(led_mutex, 0)){
        bg_color  = (led_state ? "#ffffff" : "#757575");
        xSemaphoreGive(led_mutex);
    }

    if(critical_tmp > 0)
    {
        warn_text = "<h2>!! PATIENT UNSTABLE !!</h2>";
        bg_color = "#ff0000";
        if((critical_tmp & BIT_HR) > 0) 
        {
            strcat(warn_states, "| HEART RATE |");
        }
        if((critical_tmp & BIT_TMP) > 0) 
        {
            strcat(warn_states, "| TEMPERATURE |");
        }
        if((critical_tmp & BIT_OX) > 0) 
        {
            strcat(warn_states, "| OXYGEN |");
        }
    }
    else
    {
        warn_text = "<h2>STABLE</h2>";
        warn_states[0] = 0;
    }
    

    if(xSemaphoreTake(data_string_mutex,2))
    {
        snprintf(html, sizeof(html), //AI use was here. I asked it to help me make the page more dynamic so i could add sensor data and a warning state.
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta http-equiv='refresh\' content='1;url=/'>"
        "<title>Patient Monitor</title>"
        "</head>"
        "<body style='background-color:%s; text-align:center;'>"
        "<h2>PATIENT MONITORING PORTAL</h2>"
        "<p>Warn state: <br>%s</p>"
        "<p>%s</p>"
        "<h3>Recent HR Sensor Readings</h3>"
        "<p>%s</p>"
        "<a href=\"/emt\"><button>TOGGLE EMERGENCY</button></a>"
        //"<a href=\"/led2off\"><button>CLEAR EMERGENCY</button></a>"
        "</body></html>",
        bg_color,
        warn_text,
        warn_states,
        sensor_data_string
        );
        xSemaphoreGive(data_string_mutex);
    }

    return  httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

}


esp_err_t get_req_handler(httpd_req_t *req)
{
    return send_web_page(req);
}
esp_err_t emt_handler(httpd_req_t *req)
{
    //gpio_set_level(LED_RED_PIN, 1);
    xSemaphoreGive(xButtonSem);
    return send_web_page(req);
}
httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = get_req_handler,
    .user_ctx = NULL};
httpd_uri_t uri_emt = {
    .uri = "/emt",
    .method = HTTP_GET,
    .handler = emt_handler,
    .user_ctx = NULL
};
httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers     = 1024;
    config.max_resp_headers     = 1024;
    // max_resp_headers can also be bumped if needed
    
    httpd_handle_t server = NULL;


    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_emt);
    }

    return server;
}

//                                                            tasks

//wifi task (does not count towards total tasks)
void wifi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    connect_wifi();
    vTaskDelete(NULL); //task need only run once
}

//server task (does not count towards total tasks)
void server_task(void *pvParameters)
{
    xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT, //wait for wifi connected bit
        pdFALSE,    //dont clear
        pdFALSE,    //only wait for this bit
        portMAX_DELAY   //wait till wifi is up, as long as it takes
    );
    setup_server();
    ESP_LOGI(TAG, "LED Control Web Server is running ... ...\n");
    vTaskDelete(NULL); //task need only run once
}


/*-------------------------------------------------------ENDOF : WIFI-------------------------------------------------------*/



/*-----------------------------------------------------GENERAL FUNCTIONS----------------------------------------------------*/
int springy_walker(int center, float k, int *state) // takes in a state, a center, and a "spring constant". as the displacement gets higher, the probability of moving further away from the center decreases relative to k. for sensors i want to keep around a certain vale but still have wander around a bit.
{
    int displacement = *state - center;
    float force = fabs((float)displacement * k); 
    float threshold = 0.5+(0.5*(float)force / ((float)force + 1.0f)); //base probability of 50% + something that approaches 0.5 as the force tends towards inf. basic x/(x+1)

    float prob = (float)esp_random()/(float)UINT32_MAX; //esp tru random num generator, better than rand() + srand

    //ESP_LOGV("SPRING_WALKER_FXN", "\ndisplacement: %d\nforce: %f\nthreshold: %f\nprob: %f\n", displacement, force, threshold, prob);

    if(prob > threshold) //if probability is greater than threshold, move further from the center
    {
        if(displacement > 0) *state = *state + 1;
        else *state = *state - 1;
    }
    else // if it is less, move back towards the center
    {
        if(displacement > 0) *state = *state - 1;
        else *state = *state + 1;
    }
    return 0;
}

void float_arr_to_buffer_string(float* arr, int *buffer_index) //interacts with variables buffer_index, sensor_data_tmp, sensor_data_string. Takes the buffer array and turns it into a string for HTML
{   
    char sensor_data_tmp[64];
    
    if(xSemaphoreTake(data_string_mutex,0))
    {
        sensor_data_string[0] = '\0'; //clear out the data string

        for(int i = BUFFER_SIZE-1; i >= 0; i--)
        {
            int trueindex = (*buffer_index + i) % BUFFER_SIZE;
            snprintf(sensor_data_tmp, sizeof(sensor_data_tmp), "Reading %d: %.2f<br>", BUFFER_SIZE-i, arr[trueindex]); //turn the values in the sensor data array into a list
            strcat(sensor_data_string, sensor_data_tmp);  //concatenate the list together
        }
        xSemaphoreGive(data_string_mutex);
    }
}

void buffer_update(float* arr, float update, int *buffer_index) //interacts with variables buffer_index. allows for easily cycling a sensor data buffer array.
{
    arr[*buffer_index] = update;
    *buffer_index = (*buffer_index + 1) % BUFFER_SIZE;
}


/*-------------------------------------------------ENDOF : GENERAL FUNCTIONS------------------------------------------------*/



/*-----------------------------------------------------------TASKS----------------------------------------------------------*/
//                                                     ordered by priority

//heartbeat task - blink to confirm monitor operation - priority 1 (low)
void heartbeat_task(void *pvParameters)
{
    int state = 100;

    while(1)
    {

        TickType_t heart_steart = xTaskGetTickCount();
        // turn LED on, wait until heartbeat (wait 1000ms)
        gpio_set_level(LED_GREEN_PIN, 1);
        vTaskDelayUntil(&heart_steart, pdMS_TO_TICKS(1000));
        ESP_LOGD("HEARTBEAT", "led ON : t = %d", pdMS_TO_TICKS(xTaskGetTickCount()));

        // turn LED off when no heartbeat (wait 1000ms)
        gpio_set_level(LED_GREEN_PIN, 0);
        vTaskDelayUntil(&heart_steart, pdMS_TO_TICKS(1000));
        ESP_LOGD("HEARTBEAT", "led OFF : t = %d", pdMS_TO_TICKS(xTaskGetTickCount()));
    }
}

//HR sensor task - take patient heart rate data in - priority 2
void hr_sensor_task(void *pvParameters) {
    bool was_stable = 1;
    while (1) 
    {
        TickType_t sensetime = xTaskGetTickCount();

        //read ADC value
        int val = adc1_get_raw(TDR_ADC_CHANNEL);
        float valf = ((float)val) * 200.0/8192.0;
        
        //if outside acceptable zone set HR Critical event bit
        if (valf > HR_THRESH_HI || valf < HR_THRESH_LO) 
        {
            if(was_stable == 1)
            {
                xEventGroupSetBits(critical_condition, BIT_HR);
            }

            //patient is no longer stable, removes spam
            was_stable = 0;
        }
                else
        {
            //the patient is stable
            was_stable = 1; //set their stability to 1
            
        }

        ESP_LOGV("HR_SENSOR", "output: %f", valf);

        //send the most recent value of sensor to the queue. do not block if queue is full.
        if(xQueueSendToBack(sensor_queue, &valf, 0) != pdPASS)
        {
            ESP_LOGV(TAG, "Queue is full"); //debug value if Q is full   
        }
        else
        {
            ESP_LOGV(TAG, "Queue pushed!!");
        }
        
        //wait for 50ms
        vTaskDelayUntil(&sensetime, pdMS_TO_TICKS(HEART_SENSOR_PERIOD));
            
    }
}

//temperature sensor task. priority 2 medium. detects patients temperature in degrees C. 35 +-5 is "safe" zone. closer to 37 +- 1 IRL
void temp_sensor_task(void *pvParameters)
{
    int was_stable = 1;
    int center = 35;
    float k = 0.2;
    int temp = 35;
    while(1)
    {
        if(xSemaphoreTake(springy_mutex, pdMS_TO_TICKS(5)))
        {
            springy_walker(center, k, &temp); //call springy walker for generated data
            xSemaphoreGive(springy_mutex);
        }
        if(temp > TMP_THRESH_HI || temp < TMP_THRESH_LO) //if patient temperature outside of safe zone, set critical condition event group bit temp
        {
            if(was_stable == 1)
            {
                xEventGroupSetBits(critical_condition, BIT_TMP);
                //ESP_LOGV("TMP_MONITOR", "going unstable!!!!!");

            }

            //patient is no longer stable, removes spam
            was_stable = 0;
        }
        else
        {
            //the patient is stable
            was_stable = 1; //set their stability to 1
            
        }
        ESP_LOGV("TMP_MONITOR", "%d", temp);

        vTaskDelay(pdMS_TO_TICKS(TMP_OX_SENSOR_PERIOD));
    }
}

//detects amount of dissolved oxygen in patients blood. normal value around 9 mMol/L. lower bound for safety is around 8
void oxy_sensor_task(void *pvParameters)
{
    int was_stable = 1;
    int center = 10;
    float k = 0.1;
    int sat = 10;
    while(1)
    {
        if(xSemaphoreTake(springy_mutex, pdMS_TO_TICKS(5)))
        {
            springy_walker(center, k, &sat);
            xSemaphoreGive(springy_mutex);
        }

        if( (95-sat) < OX_THRESH_LO*10)
        {
            if(was_stable == 1)
            {
                xEventGroupSetBits(critical_condition, BIT_OX);
            }

            //patient is no longer stable, removes spam
            was_stable = 0;
        }
        else
        {
            //the patient is stable
            was_stable = 1; //set their stability to 1
            
        }
        ESP_LOGV("OXY_MONITOR", "%d", 95-sat);

        vTaskDelay(pdMS_TO_TICKS(TMP_OX_SENSOR_PERIOD));
    }
}

//event handler task. handle all critical state, emergency button, ect. priority 3 (high)
void event_handler(void *pvParameters)
{
    float queue_temp;
    float sensor_data_buffer[BUFFER_SIZE];
    int buffer_index = 0;
    TickType_t handletime;
    int handle_duration;

    while(1)
    {
        handletime = xTaskGetTickCount();
        //handle event group. update bits and check
        critical_bits = xEventGroupWaitBits(
            critical_condition,
            BIT_HR | BIT_TMP | BIT_OX, //GOOGLE GEMINI WAS USED TO HELP FIND A BUG HERE. originally, logical, not bitwise operators were used. as such, the oxygen and temperature errors would get stuck on. I knew it was something here but not what. I pased the event handler section with the description of my error into google gemini and it clarified my error.
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(10)
        );

        if(critical_bits > 0)
        {
            if(xSemaphoreTake(led_mutex,pdMS_TO_TICKS(1))) //sets emergency state LED
            {
                led_state = 1;
                gpio_set_level(LED_RED_PIN, led_state);
                xSemaphoreGive(led_mutex);
            }

            if(xSemaphoreTake(critical_mutex,5))
            {
                if(critical_bits & BIT_HR)
                {
                    critical_states |= BIT_HR; //sets the critical states variable to be critical in the HR bit
                }
                if(critical_bits & BIT_TMP)
                {
                    critical_states |= BIT_TMP;
                }
                if(critical_bits & BIT_OX)
                {
                    critical_states |= BIT_OX;
                }

                xSemaphoreGive(critical_mutex);
            }
        }
        //end of event group

        //update sensor queue
        if(xQueueReceive(sensor_queue, &queue_temp, 0) == pdPASS)
        {
            buffer_update(sensor_data_buffer, queue_temp, &buffer_index);
            float_arr_to_buffer_string(sensor_data_buffer, &buffer_index);
        }

        //handle button
        if(xSemaphoreTake(xButtonSem,0))
        {
            if(xSemaphoreTake(critical_mutex,5))
            {
                if(critical_states == 0) critical_states = (BIT_HR | BIT_TMP | BIT_OX);
                else critical_states = 0;
                xSemaphoreGive(critical_mutex);
            }

            if(xSemaphoreTake(led_mutex,pdMS_TO_TICKS(5)))
            {
                led_state = !led_state;
                gpio_set_level(LED_RED_PIN, led_state);
                xSemaphoreGive(led_mutex);
            }
        }

        handle_duration = xTaskGetTickCount() - handletime;
        if(handle_duration>15) ESP_LOGV("HANDLER", "%d", handle_duration);
    }
}

//button ISR call (no priority)
void IRAM_ATTR button_isr_handler(void* pvParameters)
{

    //save current press time
    TickType_t current_press_t = xTaskGetTickCountFromISR();

    //check if the last press occured within DEBOUNCE_TIME ms of the current press
    if(current_press_t - last_press_t > pdMS_TO_TICKS(DEBOUNCE_TIME))
    {

      //log the press
      //printf("button pressed - setting semaphore");

      //create a bool, default to false, that says if we woke a task higher than the one being interrupted by the ISR
      BaseType_t xHigherPrioTaskWoken = pdFALSE;

      //create the semaphore, update the priority boolean if the semaphore woke a higher priority task than the one being interrupted
      xSemaphoreGiveFromISR(xButtonSem, &xHigherPrioTaskWoken);

      //context switch immediately if a higher priority task was woken, otherwise ISR yields to interrupted task
      portYIELD_FROM_ISR(xHigherPrioTaskWoken);

    }
    //else printf("bounce detected\n");

    last_press_t = current_press_t;

}



/*-------------------------------------------------------ENDOF : TASKS------------------------------------------------------*/

void app_main(void)
{
     esp_log_level_set(TAG, 1);
     esp_log_level_set("WIFI_EVENT_HANDLER", ESP_LOG_VERBOSE);
     esp_log_level_set("CONNECT_WIFI", ESP_LOG_VERBOSE);
     esp_log_level_set("HEARTBEAT", 4);
     esp_log_level_set("HR_SENSOR", 1);
     esp_log_level_set("TMP_MONITOR",1);
     esp_log_level_set("OXY_MONITOR",1);
     esp_log_level_set("HANDLER",5);

    //set up hardware
        //LED
            gpio_reset_pin(LED_RED_PIN);
            gpio_set_direction(LED_RED_PIN, GPIO_MODE_OUTPUT);
            gpio_reset_pin(LED_GREEN_PIN);
            gpio_set_direction(LED_GREEN_PIN, GPIO_MODE_OUTPUT);
        //TDS
            gpio_reset_pin(TDR_PIN);
            gpio_set_direction(TDR_PIN, GPIO_MODE_INPUT);
        //ADC
            adc1_config_width(ADC_WIDTH_BIT_13);
            adc1_config_channel_atten(TDR_ADC_CHANNEL, ADC_ATTEN_DB_11);
        //BTN
            gpio_reset_pin(BTN_PIN);
            gpio_set_direction(BTN_PIN, GPIO_MODE_INPUT);
            gpio_pullup_en(BTN_PIN);
            gpio_install_isr_service(0);
            gpio_set_intr_type(BTN_PIN, GPIO_INTR_NEGEDGE);
            gpio_isr_handler_add(BTN_PIN, button_isr_handler, NULL);

    //initialize sync vars and queues
        led_mutex = xSemaphoreCreateMutex();
        critical_mutex = xSemaphoreCreateMutex();
        data_string_mutex = xSemaphoreCreateMutex();
        springy_mutex = xSemaphoreCreateMutex();
        sem_web_on = xSemaphoreCreateBinary();
        sem_web_off = xSemaphoreCreateBinary();
        xButtonSem = xSemaphoreCreateBinary();

        critical_condition = xEventGroupCreate();

        sensor_queue = xQueueCreate(5, sizeof(float));

    // Initialize NVS
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

    //just to check ticks per ms
        printf("Tick Rate: %d Hz (1 tick = %lu ms)\n", configTICK_RATE_HZ, portTICK_PERIOD_MS);

    //tasks
        xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, NULL, 4, NULL, 0);

        xTaskCreatePinnedToCore(server_task, "server_setup", 4096, NULL, 3, NULL, 0); //my physical device only has 1 core. pin to core 1 if possible in all subsequent tasks.

        xTaskCreatePinnedToCore(event_handler, "handler", 4096, NULL, 5, NULL, 0);

        xTaskCreatePinnedToCore(hr_sensor_task, "heart sensor", 2048, NULL, 2, NULL, 0);

        xTaskCreatePinnedToCore(temp_sensor_task, "temp sensor", 2048, NULL, 2, NULL, 0);

        xTaskCreatePinnedToCore(oxy_sensor_task, "oxy sensor", 2048, NULL, 2, NULL, 0);

        xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL, 0);
}
