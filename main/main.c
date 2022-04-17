#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "wifi.h"

#define WIFI_SSID "LIG10-FIBRA_3047-4141_Maria"
#define WIFI_PASSWORD "2106ma91"

#define MAX_HTTP_OUTPUT_BUFFER 2048
#define MAX_HTTP_INPUT_BUFFER 2048

QueueHandle_t xQueue_button; //Fila do botão

void install_nvs_flash_service()
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {//Se não houver mais memória disponivel(cheia) ou se tiver que atualizar a versão da memória precisar ser atualizada
	  ESP_ERROR_CHECK(nvs_flash_erase()); //Limpa a memória
	  ret = nvs_flash_init(); //Reinicia o sistema
	}

	ESP_ERROR_CHECK(ret);//Caso o erro não for tratavel o código para aqui
}

void install_led_service()
{
	gpio_config_t io_led_conf = {
			.intr_type = GPIO_INTR_DISABLE, //Interrupção do led desativada
			.mode = GPIO_MODE_OUTPUT, //Define que o led é uma saída
			.pull_down_en = 0, //Pull down desabilitado
			.pull_up_en = 0,  //Pull up desabilitado
			.pin_bit_mask = (1ULL<<GPIO_NUM_2) //Led no pino 2
	};

	gpio_config(&io_led_conf);//Configura a porta do led

	gpio_set_level(GPIO_NUM_2, 0);//Configura o pino do led com set 0 para o led iniciar apagado
}

void set_level_led_service(int level)
{
	gpio_set_level(GPIO_NUM_2, level);//Verifica e determinar o nível lógico do led
}

void led_handler(cJSON *json)
{
	set_level_led_service(cJSON_GetObjectItem(json, "value")->valueint);//Pega o valor inteiro da struct
}

void install_button_service()
{
	gpio_config_t io_button_conf = {
			.intr_type = GPIO_INTR_DISABLE,//Interrupção do botão é desabilitada
			.mode = GPIO_MODE_INPUT, //Definição que o botão é uma entrada
			.pull_down_en = 1, //Ativa o pull down
			.pull_up_en = 0,   //Desativa o pull up
			.pin_bit_mask = (1ULL<<GPIO_NUM_5) //Botão no pino 4
	};

	gpio_config(&io_button_conf);//Configura a porta do botão

	gpio_set_level(GPIO_NUM_5, 0);//Set 0 para o botão iniciar em 0
}

esp_err_t  http_event_handler(esp_http_client_event_t *evt)
{
	static int output_len;       // Stores number of bytes read

	switch(evt->event_id) { //Dependendo do ip recebido tem-se uma tomada de decisão
		case HTTP_EVENT_ERROR:
			ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
			break;
		case HTTP_EVENT_ON_CONNECTED:
			ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
			break;
		case HTTP_EVENT_HEADER_SENT:
			ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
			break;
		case HTTP_EVENT_ON_HEADER:
			ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
			break;
		case HTTP_EVENT_ON_DATA:
			ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);

			if (!esp_http_client_is_chunked_response(evt->client)) {
				memcpy(evt->user_data + output_len, evt->data, evt->data_len);
				output_len += evt->data_len;
			} else {
				memcpy(evt->user_data, evt->data, evt->data_len);
			}
			break;
		case HTTP_EVENT_ON_FINISH:
			ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");

			output_len = 0;
			break;
		case HTTP_EVENT_DISCONNECTED:
			ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");

			int mbedtls_err = 0;

			esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);

			if (err != 0) {
				ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
				ESP_LOGI(TAG, "esp error: %s", esp_err_to_name(err));
				ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
				return err;
			}
			output_len = 0;
			break;
	}

	return ESP_OK;
}

esp_http_client_handle_t create_http_client(const char* url, char* response_buffer)
{
	esp_http_client_config_t config = { //Função responsavel por configurar o cliente http
		.url = url,//Acessa os recursos do servidor na url (endereço)
		.event_handler = http_event_handler,//O client usará o evente handles da struct acima
		.user_data = response_buffer,//Armazena as informações de resposta no buffer
		.buffer_size = MAX_HTTP_OUTPUT_BUFFER,//Tamanho do buffer de resposta
		.buffer_size_tx = MAX_HTTP_INPUT_BUFFER,//Tamanho do buffer de transmissão
		.disable_auto_redirect = true,//Desabilita redirecionamentos automaticos
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);//Inicializa o http cliente com as configurações setadas acima

	esp_http_client_set_header(client, "Accept", "application/json");//Adicionando o hhttp cliente um header no formato json

	return client; //Retorna ao cliente configurado
}

esp_err_t http_get(esp_http_client_handle_t client, const char* url)
{
	esp_http_client_set_url(client, url);//Configura o endereço (url) em que se deseja o get(ler informação)
	esp_http_client_set_method(client, HTTP_METHOD_GET);//Configurao cliente com url para ele trabalhar com o metodo get

	return esp_http_client_perform(client);//Realiza a requisição
}

esp_err_t http_post(esp_http_client_handle_t client, const char* url, cJSON *content)
{
	char* post_data = cJSON_Print(content);//Pega a struct json e converte para json string(parceando)

	esp_http_client_set_url(client, url);//Configura o endereço (url) em que se deseja o post(para enviar uma informação)
	esp_http_client_set_method(client, HTTP_METHOD_POST);//Configurao cliente com url para ele trabalhar com o metodo post
	esp_http_client_set_header(client, "Content-Type", "application/json");//Adiconando cabeçalho na requisição para enviar uma string no formato json
	esp_http_client_set_post_field(client, post_data, strlen(post_data));//Vincula a string enviada no http client


	return esp_http_client_perform(client);//Envia a requisição
}

void vTask_http_get(void * arg)
{
	char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

	while(1) { //Laço de repetição
		esp_http_client_handle_t client = create_http_client("http://tkth.com.br/misc/cloud/api/led/status", local_response_buffer);//Configura o endereço (url)
		esp_err_t err = http_get(client, "http://tkth.com.br/misc/cloud/api/led/status"); //Configura o cliente com url para ele trabalhar com o metodo get

		 if (err == ESP_OK) { //Se o procesimento de certo
			ESP_LOGI(
					TAG,
					"HTTP Status = %d, content_length = %d",//Imprime o status code da resposta e o tamanho do conteudo
					esp_http_client_get_status_code(client),//status code da resposta
					esp_http_client_get_content_length(client)//tamanho do conteudo
			);
		} else {//Se não
			ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));//Mostra que o correu erro
		}

		printf("response: %s\n", local_response_buffer);//Imprime a resposta (o conteudo do buffer)
		cJSON *json_response = cJSON_Parse(local_response_buffer);//Parcea o conteudo do buffer na struct jason
		led_handler(json_response);// Ler a resposta do buffer e executar a ação no led_handler
		cJSON_Delete(json_response);//Deleta a struct da memória para liberar espaço
		esp_http_client_cleanup(client);//
		vTaskDelay(10000 / portTICK_PERIOD_MS); //Delay de 10s
	}
}

void vTask_http_post(void * arg)
{
	char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
	unsigned int btn_level = 0;

	while(1) {
		if (xQueueReceive(xQueue_button, &btn_level, portMAX_DELAY)) {
			esp_http_client_handle_t client = create_http_client(
					"http://tkth.com.br/misc/cloud/api/led/status",
					local_response_buffer
			);

			cJSON *json;
			json = cJSON_CreateObject();
			cJSON_AddNumberToObject(json, "value", !btn_level);//{ "value": 0 } -

			esp_err_t err = http_post(client, "http://www.tkth.com.br/misc/cloud/api/button", json);

			 if (err == ESP_OK) {
				ESP_LOGI(
						TAG,
						"HTTP Status = %d, content_length = %d",
						esp_http_client_get_status_code(client),
						esp_http_client_get_content_length(client)
				);
			} else {
				ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
			}

			printf("response: %s\n", local_response_buffer);

			cJSON_Delete(json);
			esp_http_client_cleanup(client);

			btn_level = 0;
		}
	}
}

void kernel(void)
{
	ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");//Inicia o sta

	wifi_sta_config_t sta_config = { //Configuracões sta
			.ssid = WIFI_SSID,//Define o nome para o ssid
			.password = WIFI_PASSWORD, //Define a senha
			.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK, //Tipo de algoritmo de altenticação (roteador)
	};

	connect_to_ap_wifi_service(sta_config);//Função para conexão do wifi que valida usuario e senha

	ESP_LOGI(TAG, "WIFI_CONNECTED");//Notificação de wifi conectado

	xQueue_button = xQueueCreate(10,sizeof(unsigned int));//Fila para verificação dos comando recebidos do botão

	xTaskCreate(vTask_http_get, "vTask_http_get", 8192, NULL, 5, NULL);//Configurações do get, memória e proridade
	xTaskCreate(vTask_http_post, "vTask_http_post", 8192, NULL, 5, NULL);//Configurações do get, memória e proridade

	while(1) {  //Laço de repetição
		unsigned int btn_level = gpio_get_level(GPIO_NUM_5);//Variavel para verificar o nível lógico do botão

		xQueueSend(xQueue_button, (void *)&btn_level, portMAX_DELAY); //A cada 5s o nivél lógico do botão é lido e enviado para a fila

		vTaskDelay(5000 / portTICK_PERIOD_MS);//Delay de 5s
	}
}

void boot(void)
{
	install_nvs_flash_service(); //Inicialização dos serviços na mémoria flash para armazenar informações mesmo que o ESP seja inicializado

	ESP_ERROR_CHECK(esp_netif_init());//Inicializa o TCP e verifica se tem erro nesse processo
	ESP_ERROR_CHECK(esp_event_loop_create_default());//Cria um loop de eventos

	install_in_station_mode_wifi_service();//Instala as configurações do wifi
	install_led_service(); //Instala as configurações do led
	install_button_service(); //Instala as configurações do wifi

	kernel(); //Chama o kernel
}

void app_main(void) //Void inicial
{
	boot(); //Chama o boot
}
