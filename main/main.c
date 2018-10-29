#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "soc/uart_struct.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "../../json/include/cJSON.h"

#define BUF_SIZE		1024
#define SSEND_MAX		1500
#define WIFI_MAX		15
#define UART_MODEM_TXD	32
#define UART_MODEM_RXD	33
#define UART_MODEM_RTS	25
#define UART_MODEM_CTS	26
#define LOC_UPDATE_INT	60000
#define MODEM_STARTUP	16000

#define EXAMPLE_ESP_WIFI_MODE_AP  	CONFIG_ESP_WIFI_MODE_AP //TRUE:AP FALSE:STA
#define EXAMPLE_ESP_WIFI_SSID      	"ESP32"
#define EXAMPLE_ESP_WIFI_PASS      	"test1234"
#define EXAMPLE_MAX_STA_CONN       	1

#define RECEIVER_IP_ADDR 			"192.168.1.1"
#define G3_IP_ADDR 					"192.168.1.2"

#define GPIO_OUTPUT_G3				18

#define DEVICE_NAME_LEN				8

#define START_BYTE					0x1f
#define STOP_BYTE					0x1a
#define DATA_BYTE					0x22
#define UNKNOWN_DATA_BYTE			0x23
#define HOST_BCAST_BYTE				0x24
#define WIFI_STAT_BYTE				0x25

typedef struct Location
{
	double longitude;
	double latitude;
	double elevation;
	int accuracy;
} Location;

static const char *TAG = "uart_sample";
static const char *FLOW_HOST = "run-west.att.io";
static const char *GET_COORD = "/9aa4487b212af/4d3e4b87dcaa/ec1d1b3fbb092dd/in/flow/getCoordinates";
static const char *LOC_POST = "/9aa4487b212af/4d3e4b87dcaa/ec1d1b3fbb092dd/in/flow/updateLocation";
static const char *MODEL = "Plus";
static const char CHAR_SUB = 0x1A;

static bool pattern_detected = false;
static bool json_resp_expected = false;
static int uart_num_pc;
static int uart_num_modem;
static char *expectedRespStr;
static char http_resp[BUF_SIZE];
static int s;

static TaskHandle_t xTask1 = NULL;
static TaskHandle_t xTask2 = NULL;
static TaskHandle_t xTask3 = NULL;
static TaskHandle_t *taskPtr = NULL;

static QueueHandle_t uart0_queue;

static TimerHandle_t location_update_timer;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

static int g3_status = 0;
static char device_name[DEVICE_NAME_LEN];

static void uart_task(void *pvParameters);
static void uart_evt_test(void *pvParameters);
static void uart_echo_test(void *pvParameters);
static void wifi_scanAP(void *pvParameters);
esp_err_t event_handler(void *ctx, system_event_t *event);

static void waitForModemResponse(const char *command, char *expectedResp)
{
	if(strlen(command) <= SSEND_MAX)
	{
		ESP_LOGI(TAG, "expectedResp: %s", expectedResp);
		expectedRespStr = expectedResp;

		uart_write_bytes(uart_num_modem, command, strlen(command));
		ESP_LOGI(TAG, "Sent");
		ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
	}
	else
	{
		ESP_LOGI(TAG, "Data too large: %d", strlen(command));
	}
}

//static void

static void uart_task(void *pvParameters)
{
    int uart_num = (int) pvParameters;
    uart_event_t event;
    size_t buffered_size;
    uint16_t total_size = 0;
    uint16_t buff_ind = 0;
    uint8_t* dtmp = (uint8_t*) malloc(BUF_SIZE);

    for(;;) {
        //Waiting for UART event.
        if(xQueueReceive(uart0_queue, (void * )&event, (portTickType)portMAX_DELAY)) {
            //ESP_LOGI(TAG, "uart[%d] event:", uart_num);
            switch(event.type) {
                //Event of UART receving data
                /*We'd better handler data event fast, there would be much more data events than
                other types of events. If we take too much time on data event, the queue might
                be full.
                in this example, we don't process data in event, but read data outside.*/
                case UART_DATA:
                    uart_get_buffered_data_len(uart_num, &buffered_size);
                    ESP_LOGI(TAG, "data, len: %d; buffered len: %d", event.size, buffered_size);
                    total_size = buffered_size;
                    break;
                //Event of HW FIFO overflow detected
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "hw fifo overflow\n");
                    //If fifo overflow happened, you should consider adding flow control for your application.
                    //We can read data out out the buffer, or directly flush the rx buffer.
                    uart_flush(uart_num);
                    break;
                //Event of UART ring buffer full
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "ring buffer full\n");
                    //If buffer full happened, you should consider encreasing your buffer size
                    //We can read data out out the buffer, or directly flush the rx buffer.
                    uart_flush(uart_num);
                    break;
                //Event of UART RX break detected
                case UART_BREAK:
                    ESP_LOGI(TAG, "uart rx break\n");
                    break;
                //Event of UART parity check error
                case UART_PARITY_ERR:
                    ESP_LOGI(TAG, "uart parity error\n");
                    break;
                //Event of UART frame error
                case UART_FRAME_ERR:
                    ESP_LOGI(TAG, "uart frame error\n");
                    break;
                //UART_PATTERN_DET
                case UART_PATTERN_DET:
                    ESP_LOGI(TAG, "uart pattern detected\n");
                    pattern_detected = true;
                    break;
                //Others
                default:
                    ESP_LOGI(TAG, "uart event type: %d\n", event.type);
                    break;
            }
        }

        if(total_size > 0)
        {
        	uint16_t bytes_recvd = 0;
        	while(bytes_recvd < total_size)
        	{
        		int len = uart_read_bytes(uart_num, &dtmp[buff_ind], total_size, 100 / portTICK_RATE_MS);
        		if(len > 0)
        		{
        			//ESP_LOGI(TAG, "uart read : %d, %.*s", len, len, (const char*)dtmp);
        			buff_ind += len;
        			bytes_recvd += len;
        			//ESP_LOGI(TAG, "buff_ind : %d", buff_ind);
        		}
        		else
        		{
        			ESP_LOGI(TAG, "uart read error");
        		}
        	}

        	if (bytes_recvd == 52) {
        		printf("Send numeric data request\n");

        		// Map host IP to ESP32
        		dtmp[28] = 192;
        		dtmp[29] = 168;
        		dtmp[30] = 1;
        		dtmp[31] = 1;

				struct sockaddr_in g3Address;
				g3Address.sin_family = AF_INET;
				g3Address.sin_addr.s_addr = inet_addr(G3_IP_ADDR);
				g3Address.sin_port = htons(8300);

				int sent_data = sendto(s, dtmp, bytes_recvd, 0, (struct sockaddr*)&g3Address, sizeof(g3Address));
				if (sent_data < 0) {
					printf("send failed\n");
				}
				else {
					printf("sent to G3: %d\n", sent_data);
					printf("send_buf: ");
					for (int i=0; i<sent_data; i++) {
						printf("%02x,", dtmp[i]);
					}
					printf("\n");
				}
        	}
        	else {

        	}

        	/*
        	if(strstr((const char*)dtmp, expectedRespStr))
        	{
        		ESP_LOGI(TAG, "Expected resp rcved");

        		if(json_resp_expected)
        		{
        			memcpy(&http_resp, dtmp, BUF_SIZE);
        		}

        		if(taskPtr != NULL)
        		{
        			xTaskNotifyGive(*taskPtr);
        		}
        	}
        	*/

        	buff_ind = 0;
        	total_size = 0;
        }

        if(!json_resp_expected)
        {
        	memset(dtmp, 0, BUF_SIZE * sizeof(uint8_t));
        	buff_ind = 0;
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

static void timer_callback(TimerHandle_t xTimer)
{
	//ESP_LOGI(TAG, "Timer callback()");
	xTaskCreate(wifi_scanAP, "wifi_scanAP", 2048, NULL, 10, &xTask3);
	taskPtr = &xTask3;
}

static void uart_evt_test(void *pvParameters)
{
	uart_num_pc = UART_NUM_0;
    uart_num_modem = UART_NUM_1;

    uart_config_t uart_config_modem = {
       .baud_rate = 115200,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .rx_flow_ctrl_thresh = 122,
    };

    uart_config_t uart_config_pc = {
       .baud_rate = 115200,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .rx_flow_ctrl_thresh = 122,
    };

    //Set UART parameters
    uart_param_config(uart_num_modem, &uart_config_modem);
    uart_param_config(uart_num_pc, &uart_config_pc);

    //Set UART log level
    esp_log_level_set(TAG, ESP_LOG_INFO);

    //Set UART pins,(-1: default pin, no change.)
    //For UART0, we can just use the default pins.
    uart_set_pin(uart_num_pc, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    //Set UART1 pins(TX: IO14, RX: I012, RTS: IO11, CTS: IO6)
    uart_set_pin(uart_num_modem, UART_MODEM_TXD, UART_MODEM_RXD, UART_MODEM_RTS, UART_MODEM_CTS);

    //Install UART driver, and get the queue.
    uart_driver_install(uart_num_modem, BUF_SIZE * 2, BUF_SIZE * 2, 10, &uart0_queue, 0);
    uart_driver_install(uart_num_pc, BUF_SIZE * 2, BUF_SIZE * 2, 10, NULL, 0);

    //Set uart pattern detect function.
    //uart_enable_pattern_det_intr(uart_num_modem, '\r', 1, 10000, 10, 10);

    //Create a task to handler UART event from ISR
    xTaskCreate(uart_task, "uart_task", 2048, (void*)uart_num_modem, 12, &xTask1);

    //Wait for modem
    /*
    vTaskDelay(MODEM_STARTUP / portTICK_PERIOD_MS);

    waitForModemResponse("AT\r", "OK");
    waitForModemResponse("ATZ\r", "OK");
    waitForModemResponse("AT+CMEE=2\r", "OK");
    waitForModemResponse("AT+IPR?\r", "OK");
    waitForModemResponse("AT#SIMDET=1\r", "OK");
    waitForModemResponse("AT#SGACT=1,0\r", "OK");
    waitForModemResponse("AT+CGDCONT=1,\"IP\",\"broadband\"\r", "OK");
    waitForModemResponse("AT+CSQ\r", "OK");
    waitForModemResponse("AT+CGMR\r", "OK");
    waitForModemResponse("AT#SGACT=1,1\r", "OK");
    waitForModemResponse("AT+CGREG?\r", "0,1");

    xTaskCreate(wifi_scanAP, "wifi_scanAP", 2048, NULL, 10, &xTask3);
    taskPtr = &xTask3;

    location_update_timer = xTimerCreate("Timer", LOC_UPDATE_INT/portTICK_PERIOD_MS, pdTRUE, (void *)0, timer_callback);
    if(location_update_timer != NULL)
    {
    	if(xTimerStart(location_update_timer, 0) != pdPASS)
    	{
    		ESP_LOGI(TAG, "Cannot start timer");
    	}
    }
    */

    vTaskDelete(NULL);
}

static char *getJsonResp(char *httpResp)
{
	char *resp;
	char *firstChar;
	char *lastChar;
	uint16_t firstInd;
	uint16_t lastInd;
	uint16_t len;

	firstChar = strchr(httpResp, '{');
	lastChar = strrchr(httpResp, '}');
	firstInd = firstChar - httpResp;
	lastInd = lastChar - httpResp;
	len = (lastInd-firstInd) + 1;

	resp = malloc(len * sizeof(char));
	strncpy(resp, &httpResp[firstInd], len);
	resp[len] = '\0';

	return resp;
}

static void parseCoord(char *jsonResp, Location *loc)
{
	cJSON *root = cJSON_Parse(jsonResp);
	cJSON *location = cJSON_GetObjectItem(root, "location");
	double lng = cJSON_GetObjectItem(location, "lng")->valuedouble;
	double lat = cJSON_GetObjectItem(location, "lat")->valuedouble;
	int accuracy = cJSON_GetObjectItem(root, "accuracy")->valueint;

	ESP_LOGI(TAG, "lng: %f, lat: %f, accuracy: %d", lng, lat, accuracy);
	loc->longitude = lng;
	loc->latitude = lat;
	loc->accuracy = accuracy;

	cJSON_Delete(root);
}

static char *http_postStr(const char *endpoint, const char *host, char *jsonData)
{
	char *postStr = malloc((200 + strlen(jsonData)) * sizeof(char));
	sprintf(postStr,
				"POST %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Content-Type: application/json\r\n"
				"Content-Length: %d\r\n\r\n"
				"%s", endpoint, host, strlen(jsonData), jsonData);

	return postStr;
}

static void update_location(char *json)
{
	char *postStr = http_postStr(GET_COORD, FLOW_HOST, json);
	//ESP_LOGI(TAG, "postStr len: %d", strlen(postStr));

	waitForModemResponse("AT#SD=1,0,80,\"run-west.att.io\",0,1,1\r", "OK");
	waitForModemResponse("AT#SSEND=1\r", ">");
	uart_write_bytes(uart_num_modem, postStr, strlen(postStr));
	waitForModemResponse(&CHAR_SUB, "SRING: 1");

	json_resp_expected = true;
	waitForModemResponse("AT#SRECV=1,500\r", "}");
	json_resp_expected = false;
	//ESP_LOGI(TAG, "http_resp: %s", http_resp);
	char *jsonResp = getJsonResp(http_resp);
	//ESP_LOGI(TAG, "jsonResp: %s", jsonResp);

	Location curr_loc;
	parseCoord(jsonResp, &curr_loc);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "longitude", curr_loc.longitude);
	cJSON_AddNumberToObject(root, "latitude", curr_loc.latitude);
	cJSON_AddNumberToObject(root, "elevation", 0);
	cJSON_AddNumberToObject(root, "accuracy", curr_loc.accuracy);
	cJSON_AddStringToObject(root, "model", MODEL);
	char *jsonStr = cJSON_Print(root);

	free(postStr);
	postStr = http_postStr(LOC_POST, FLOW_HOST, jsonStr);

	waitForModemResponse("AT#SSEND=1\r", ">");
	uart_write_bytes(uart_num_modem, postStr, strlen(postStr));
	waitForModemResponse(&CHAR_SUB, "SRING: 1");
	waitForModemResponse("AT#SRECV=1,500\r", "}");

	waitForModemResponse("AT#SH=1\r", "OK");

	taskPtr = NULL;
	free(jsonResp);
	free(jsonStr);
	free(postStr);
	cJSON_Delete(root);
}

static void wifi_scanAP(void *pvParameters)
{
	uint16_t num_aps;
	wifi_scan_config_t scan_config = {
		.show_hidden = true
	};

	ESP_LOGI(TAG, "Start AP scan");
	ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&num_aps));
	ESP_LOGI(TAG, "Scan done. Found %d APs", num_aps);

	if(num_aps > WIFI_MAX)
	{
		num_aps = WIFI_MAX;
	}

	wifi_ap_record_t *aps = malloc(num_aps * sizeof(wifi_ap_record_t));
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&num_aps, aps));

	cJSON *root = cJSON_CreateObject();
	cJSON *wifiArr;
	cJSON *wifiAp;
	cJSON_AddItemToObject(root, "wifiAccessPoints", wifiArr = cJSON_CreateArray());

	for(int i=0; i<num_aps; i++)
	{
		wifi_ap_record_t ap = aps[i];
		char *bssidStr = malloc(18 * sizeof(char));
		sprintf(bssidStr, "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5]);
		//ESP_LOGI(TAG, "AP %d: %s, %d", i, bssidStr, ap.rssi);

		cJSON_AddItemToArray(wifiArr, wifiAp = cJSON_CreateObject());
		cJSON_AddStringToObject(wifiAp, "macAddress", bssidStr);
		cJSON_AddNumberToObject(wifiAp, "signalStrength", ap.rssi);

		free(bssidStr);
	}

	char *jsonString = cJSON_Print(root);
	//ESP_LOGI(TAG, "jsonString: %s", jsonString);
	update_location(jsonString);

	free(jsonString);
	free(aps);
	cJSON_Delete(root);
	vTaskDelete(NULL);
}

//an example of echo test with hardware flow control on UART1
static void uart_echo_test(void *pvParameters)
{
    int uart_num = UART_NUM_1;
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };
    //Configure UART1 parameters
    uart_param_config(uart_num, &uart_config);
    //Set UART1 pins(TX: IO4, RX: I05, RTS: IO18, CTS: IO19)
    uart_set_pin(uart_num, UART_MODEM_TXD, UART_MODEM_RXD, UART_MODEM_RTS, UART_MODEM_CTS);
    //Install UART driver( We don't need an event queue here)
    //In this example we don't even use a buffer for sending data.
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);

    uint8_t* data = (uint8_t*) malloc(BUF_SIZE);
    while(1) {
        //Read data from UART
        int len = uart_read_bytes(uart_num, data, BUF_SIZE, 20 / portTICK_RATE_MS);
        //Write data back to UART
        uart_write_bytes(uart_num, (const char*) data, len);
    }
}

void wifi_init_softap()
{
    wifi_event_group = xEventGroupCreate();

    //ESP_LOGI(TAG, "Here6")
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    //ESP_LOGI(TAG, "Here7")

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    //ESP_LOGI(TAG, "Here8")
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    //ESP_LOGI(TAG, "Here2")
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    //ESP_LOGI(TAG, "Here3")
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    //ESP_LOGI(TAG, "Here4")
    ESP_ERROR_CHECK(esp_wifi_start());
    //ESP_LOGI(TAG, "Here5")

    ESP_LOGI(TAG, "wifi_init_softap finished.");
}

esp_err_t event_handler(void *ctx, system_event_t *event)
{
	ESP_LOGI(TAG, "System event rcved ID: %d", event->event_id);

	switch(event->event_id) {
	    case SYSTEM_EVENT_STA_START:
	        esp_wifi_connect();
	        break;
	    case SYSTEM_EVENT_STA_GOT_IP:
	        ESP_LOGI(TAG, "got ip:%s",
	                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
	        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
	        break;
	    case SYSTEM_EVENT_AP_STACONNECTED:
	        ESP_LOGI(TAG, "station:"MACSTR" join, AID=%d",
	                 MAC2STR(event->event_info.sta_connected.mac),
	                 event->event_info.sta_connected.aid);

	        g3_status = 1;
	        gpio_set_level(GPIO_OUTPUT_G3, g3_status);
	        break;
	    case SYSTEM_EVENT_AP_STADISCONNECTED:
	        ESP_LOGI(TAG, "station:"MACSTR" leave, AID=%d",
	                 MAC2STR(event->event_info.sta_disconnected.mac),
	                 event->event_info.sta_disconnected.aid);

	        g3_status = 0;
	        gpio_set_level(GPIO_OUTPUT_G3, g3_status);
	        break;
	    case SYSTEM_EVENT_STA_DISCONNECTED:
	        esp_wifi_connect();
	        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
	        break;
	    default:
	        break;
	    }

	    return ESP_OK;
}

static void udpData_listen_task(void *pvParameters)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	ESP_LOGI(TAG, "Data New socket = %d\n", s);

	struct sockaddr_in serverAddress;
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = inet_addr(RECEIVER_IP_ADDR);
	serverAddress.sin_port = htons(8301);
	int rc = bind(s, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
	//ESP_LOGI(TAG, "Data bind() = %d\n", rc);
	printf("Bind to Port Number %d ,IP address %s\n",8301,RECEIVER_IP_ADDR);

	//rc = listen(s, 10);
	//ESP_LOGI(TAG, "Data listen() = %d\n", rc);

	struct sockaddr_in clientAddress;
	socklen_t clientAddressLength = sizeof(clientAddress);
	int r;
	int num_vs;
	int index;
	int total_len;
	char recv_buf[300];
	char send_buf[300];

	while (1) {
		//int clientSock = accept(s, (struct sockaddr *)&clientAddress, &clientAddressLength);
		//ESP_LOGI(TAG, "Data accept() = %d\n", clientSock);

		//set O_NONBLOCK so that recv will return, otherwise we need to impliment message end
		//detection logic. If know the client message format you should instead impliment logic
		//detect the end of message

		do {
			bzero(recv_buf, sizeof(recv_buf));
			//ESP_LOGI(TAG, "recv() 1");
			r = recv(s,recv_buf,sizeof(recv_buf),0);
			printf("Recv %d bytes\n", r);
			//ESP_LOGI(TAG, "recv() 2");
			printf("udpData recv(): ");
			for (int i = 0; i < r; i++) {
				printf("%02x,", recv_buf[i]);
				//putchar(recv_buf[i]);
			}
			printf("\n");
			//uart_write_bytes(uart_num_modem, recv_buf, r);

			num_vs = recv_buf[82];
			printf("num_vs: %d\n", num_vs);
			if (num_vs > 0 && r > 270) {
				index = 84;

				for (int i=0; i<num_vs; i++) {
					//printf("Key: %d%d\n", recv_buf[index], recv_buf[index+1]);
					index += 10;
				}

				index = 84;
				total_len = r + 3 + DEVICE_NAME_LEN;
				memset(send_buf, 0, sizeof(send_buf));
				send_buf[0] = START_BYTE;
				send_buf[1] = DATA_BYTE;
				memcpy(&send_buf[2], recv_buf, r);
				memcpy(&send_buf[2+r], device_name, DEVICE_NAME_LEN);
				send_buf[total_len-1] = STOP_BYTE;

				printf("send_buf: %d\n", total_len);
				for (int i=0; i<total_len; i++) {
					printf("%02x,", send_buf[i]);
				}
				printf("\n");

				uart_write_bytes(uart_num_modem, send_buf, total_len);
			}
			else if (r > 0 && r < 270) {
				total_len = r + 3;
				memset(send_buf, 0, sizeof(send_buf));
				send_buf[0] = START_BYTE;
				send_buf[1] = UNKNOWN_DATA_BYTE;
				memcpy(&send_buf[2], recv_buf, r);
				send_buf[total_len-1] = STOP_BYTE;

				printf("send_buf: %d\n", total_len);
				for (int i=0; i<total_len; i++) {
					printf("%02x,", send_buf[i]);
				}
				printf("\n");

				uart_write_bytes(uart_num_modem, send_buf, total_len);
			}
		} while(r > 0);
		ESP_LOGI(TAG, "Data ... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);

		/*
		if( write(clientSock , MESSAGE , strlen(MESSAGE)) < 0)
		{
			ESP_LOGE(TAG, "... Send failed \n");
			//close(s);
			vTaskDelay(4000 / portTICK_PERIOD_MS);
		}
		ESP_LOGI(TAG, "... socket send success");
		*/
		//close(clientSock);
	}

	vTaskDelete(NULL);
}

static void udpNet9_listen_task(void *pvParameters)
{
	s = socket(AF_INET, SOCK_DGRAM, 0);
	ESP_LOGI(TAG, "Net9 New socket = %d\n", s);

	struct sockaddr_in serverAddress;
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = inet_addr(RECEIVER_IP_ADDR);
	serverAddress.sin_port = htons(8300);
	int rc = bind(s, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
	printf("Bind to Port Number %d ,IP address %s\n",8300,RECEIVER_IP_ADDR);

	//rc = listen(s, 10);
	//ESP_LOGI(TAG, "Net9 listen() = %d\n", rc);

	struct sockaddr_in clientAddress;
	socklen_t clientAddressLength = sizeof(clientAddress);
	int r;
	int sent_data;
	char recv_buf[128];
	unsigned char send_buf[52];
	char pi_send_buf[80];
	int total_len = 0;

	while (1) {
		//int clientSock = accept(s, (struct sockaddr *)&clientAddress, &clientAddressLength);
		//ESP_LOGI(TAG, "Net9 accept() = %d\n", clientSock);

		//set O_NONBLOCK so that recv will return, otherwise we need to impliment message end
		//detection logic. If know the client message format you should instead impliment logic
		//detect the end of message

		do {
			bzero(recv_buf, sizeof(recv_buf));
			r = recv(s,recv_buf,sizeof(recv_buf),0);
			printf("udpNet9 recv(): ");
			for (int i = 0; i < r; i++) {
				printf("%02x,", recv_buf[i]);
				//putchar(recv_buf[i]);
			}
			printf("\n");

			if ((recv_buf[0] == 4) && (recv_buf[1] == 255)) {
				printf("HostBcast: Received host broadcast\n");

				total_len = r + 3;
				memset(pi_send_buf, 0, sizeof(pi_send_buf));
				pi_send_buf[0] = START_BYTE;
				pi_send_buf[1] = HOST_BCAST_BYTE;
				memcpy(&pi_send_buf[2], recv_buf, r);
				pi_send_buf[total_len-1] = STOP_BYTE;

				printf("send_buf: %d\n", total_len);
				for (int i=0; i<total_len; i++) {
					printf("%02x,", pi_send_buf[i]);
				}
				printf("\n");

				uart_write_bytes(uart_num_modem, pi_send_buf, total_len);

				bzero(send_buf, sizeof(send_buf));
				// [0,0,0,28,0,52,0,1,0,1,0,0,2,28,0,0,0,40,0,0,0,0,0,0,int(bip[0]),int(bip[1]),int(bip[2]),int(bip[3]),int(hip[0]),int(hip[1]),int(hip[2]),int(hip[3]),1,0,0,0,1,0,0,0,85,85,85,127,1,0,0,0,85,85,85,95]
				// bip[0] -> 24 - 27
				// hip[0] -> 28 - 31
				memcpy(device_name, &recv_buf[36], 8);
				//device_name[8] = '\0';
				printf("Device name: %s\n", device_name);

				send_buf[3] = 28;
				send_buf[5] = 52;
				send_buf[7] = 1;
				send_buf[9] = 1;
				send_buf[12] = 2;
				send_buf[13] = 28;
				send_buf[17] = 40;
				// bed ip
				send_buf[24] = recv_buf[52];
				send_buf[25] = recv_buf[53];
				send_buf[26] = recv_buf[54];
				send_buf[27] = recv_buf[55];
				// host ip
				send_buf[28] = 192;
				send_buf[29] = 168;
				send_buf[30] = 1;
				send_buf[31] = 1;

				send_buf[32] = 1;
				send_buf[36] = 1;
				send_buf[40] = 85;
				send_buf[41] = 85;
				send_buf[42] = 85;
				send_buf[43] = 127;
				send_buf[44] = 1;
				send_buf[48] = 85;
				send_buf[49] = 85;
				send_buf[50] = 85;
				send_buf[51] = 95;

				printf("send_buf: ");
				for (int i=0; i<sizeof(send_buf); i++) {
					printf("%02x,", send_buf[i]);
				}
				printf("\n");

				struct sockaddr_in g3Address;
				g3Address.sin_family = AF_INET;
				g3Address.sin_addr.s_addr = inet_addr(G3_IP_ADDR);
				g3Address.sin_port = htons(8300);

				sent_data = sendto(s, send_buf, sizeof(send_buf), 0, (struct sockaddr*)&g3Address, sizeof(g3Address));
				if (sent_data < 0) {
					printf("send failed\n");
				}
				else {
					printf("sent req: %d\n", sent_data);
				}

			}

			//uart_write_bytes(uart_num_modem, recv_buf, r);
		} while(r > 0);
		ESP_LOGI(TAG, "Net9 ... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);

		/*
		if( write(clientSock , MESSAGE , strlen(MESSAGE)) < 0)
		{
			ESP_LOGE(TAG, "... Send failed \n");
			//close(s);
			vTaskDelay(4000 / portTICK_PERIOD_MS);
		}
		ESP_LOGI(TAG, "... socket send success");
		*/
		//close(clientSock);
	}

	vTaskDelete(NULL);
}

static void start_dhcp_server()
{
	// initialize the tcp stack
	tcpip_adapter_init();
	// stop DHCP server
	ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
	// assign a static IP to the network interface
	tcpip_adapter_ip_info_t info;
	memset(&info, 0, sizeof(info));
	IP4_ADDR(&info.ip, 192, 168, 1, 1);
	IP4_ADDR(&info.gw, 192, 168, 1, 1);//ESP acts as router, so gw addr will be its own addr
	IP4_ADDR(&info.netmask, 255, 255, 255, 0);
	ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));
	// start the DHCP server
	ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
	printf("DHCP server started \n");
}

void app_main(void)
{
    nvs_flash_init();
    start_dhcp_server();
    //ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    ESP_LOGI(TAG, "Here1")

    /*
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    */
    wifi_init_softap();
    xTaskCreate(udpData_listen_task, "udpData_listen_task", 4096, NULL, 10, NULL);
    xTaskCreate(udpNet9_listen_task, "udpNet9_listen_task", 2048, NULL, 10, NULL);

    //A uart read/write example without event queue;
    //xTaskCreate(uart_echo_test, "uart_echo_test", 1024, NULL, 10, NULL);

    //A uart example with event queue.
    xTaskCreate(uart_evt_test, "uart_evt_test", 2048, NULL, 10, &xTask2);
    taskPtr = &xTask2;

    gpio_set_direction(GPIO_OUTPUT_G3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_OUTPUT_G3, g3_status);
/*
    while (true) {

        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    */
}
