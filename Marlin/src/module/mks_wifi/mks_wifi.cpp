#include "mks_wifi.h"


#ifdef MKS_WIFI

#include "../../lcd/marlinui.h"
#include "mks_wifi_sd.h"
#include "../../lcd/tft/qqs/qqs_wifi_diag.h"
#include "qqs_espflash.h"

qqs_wifi_diag_t qqs_wdiag;
qqs_wifi_net_t qqs_wnet;
qqs_wifi_scan_t qqs_wscan;

// ---- QQS-Pro: network setup from the printer screen (MKS ESP protocol, see MKS-WIFI MksWifi.ino) ----

// One raw frame: A5 type lenL lenH data FC (no CR LF, unlike mks_wifi_build_packet for non-NET types)
static void qqs_wifi_frame(const uint8_t type, const uint8_t *d, const uint16_t len) {
	static uint8_t buf[4 + 1 + 1 + 32 + 1 + 64 + 1];
	if (len > sizeof(buf) - 5) return;
	buf[0] = ESP_PROTOC_HEAD;
	buf[1] = type;
	buf[2] = len & 0xFF;
	buf[3] = len >> 8;
	if (len) memcpy(&buf[4], d, len);
	buf[4 + len] = ESP_PROTOC_TAIL;
	mks_wifi_send(buf, 4 + len);          // sends size + 1 bytes (the tail included)
}

void qqs_wifi_config(const uint8_t mode, const char *ssid, const char *key) {
	uint8_t d[1 + 1 + 32 + 1 + 64];
	const uint8_t nl = _MIN(strlen(ssid), (size_t)32), kl = _MIN(strlen(key), (size_t)64);
	d[0] = mode;
	d[1] = nl;
	memcpy(&d[2], ssid, nl);
	d[2 + nl] = kl;
	memcpy(&d[3 + nl], key, kl);
	qqs_wifi_frame(ESP_TYPE_NET, d, 3 + nl + kl);
	if (mode == WIFI_MODE_STA) {          // same settings as before = no restart; then this starts the connection
		const uint8_t c = 0x01;
		qqs_wifi_frame(0x09, &c, 1);
	}
}

void qqs_wifi_scan() { qqs_wifi_frame(0x07, nullptr, 0); }

void qqs_wifi_reset() {
	WRITE(MKS_WIFI_IO_RST, LOW);
	safe_delay(200);
	WRITE(MKS_WIFI_IO_RST, HIGH);
}

// Type 0 from the ESP: ip[4] port[2] state mode nameLen name keyLen key ...
static void qqs_net_parse(const uint8_t *d, const uint16_t len) {
	if (len < 10) return;
	const uint8_t nl = d[8];
	if (nl > 32 || 10u + nl > len) return;
	const uint8_t kl = d[9 + nl];
	if (kl > 64 || 10u + nl + kl > len) return;
	qqs_wnet.state = d[6];
	qqs_wnet.mode = d[7];
	memcpy(qqs_wnet.ssid, &d[9], nl);  qqs_wnet.ssid[nl] = 0;
	memcpy(qqs_wnet.key, &d[10 + nl], kl); qqs_wnet.key[kl] = 0;
	qqs_wnet.reports++;
}

// Type 4 from the ESP: count, then per network: nameLen name rssi(int8)
static void qqs_list_parse(const uint8_t *d, const uint16_t len) {
	if (!len) return;
	qqs_wifi_scan_t &s = qqs_wscan;
	uint8_t n = 0;
	uint16_t i = 1;
	for (uint8_t k = 0; k < d[0] && i < len; k++) {
		const uint8_t l = d[i++];
		if (l > 32 || i + l + 1u > len) break;
		const uint8_t *nm = &d[i];
		i += l;
		const int8_t r = (int8_t)d[i++];
		if (!l) continue;                   // hidden network
		int8_t same = -1;
		for (uint8_t j = 0; j < n; j++)
			if (!strncmp(s.name[j], (const char *)nm, l) && !s.name[j][l]) { same = j; break; }
		if (same >= 0) { if (r > s.rssi[same]) s.rssi[same] = r; continue; }
		if (n >= QQS_WSCAN_MAX) continue;
		memcpy(s.name[n], nm, l); s.name[n][l] = 0;
		s.rssi[n] = r;
		n++;
	}
	// strongest first
	for (uint8_t a = 1; a < n; a++)
		for (uint8_t b = a; b && s.rssi[b] > s.rssi[b - 1]; b--) {
			char t[33]; memcpy(t, s.name[b], 33); memcpy(s.name[b], s.name[b - 1], 33); memcpy(s.name[b - 1], t, 33);
			const int8_t q = s.rssi[b]; s.rssi[b] = s.rssi[b - 1]; s.rssi[b - 1] = q;
		}
	s.n = n;
	s.seq++;
}

static void qqs_diag_copy(char *dst, const char *src) {   // 27 printable chars max
	uint8_t i = 0;
	for (; i < 27 && src[i]; i++) dst[i] = (src[i] < 0x20 || src[i] > 0x7E) ? '?' : src[i];
	dst[i] = 0;
}

volatile uint8_t mks_in_buffer[MKS_IN_BUFF_SIZE];
uint8_t mks_out_buffer[MKS_OUT_BUFF_SIZE];

volatile uint8_t esp_packet[MKS_TOTAL_PACKET_SIZE];


void mks_wifi_init(void){

	SERIAL_ECHO_MSG("Init MKS WIFI");	
    DEBUG("Init MKS WIFI");
	
	// SET_OUTPUT(MKS_WIFI_IO0);
	// WRITE(MKS_WIFI_IO0, HIGH);

	SET_OUTPUT(MKS_WIFI_IO4);
	WRITE(MKS_WIFI_IO4, HIGH);

	SET_OUTPUT(MKS_WIFI_IO_RST);
	WRITE(MKS_WIFI_IO_RST, LOW);

	ui.set_status((const char *)"WIFI: waiting... ",false);

	safe_delay(200);	
	WRITE(MKS_WIFI_IO_RST, HIGH);
	
	safe_delay(200);	
	WRITE(MKS_WIFI_IO4, LOW);
}


void mks_wifi_set_param(void){
	uint32_t packet_size;
	ESP_PROTOC_FRAME esp_frame;


	uint32_t ap_len = strlen((const char *)MKS_WIFI_SSID);
	uint32_t key_len = strlen((const char *)MKS_WIFI_KEY);


	memset(mks_out_buffer, 0, MKS_OUT_BUFF_SIZE);

	mks_out_buffer[0] = WIFI_MODE_STA;

	mks_out_buffer[1] = ap_len;
	memcpy((char *)&mks_out_buffer[2],(const char *)MKS_WIFI_SSID,ap_len);

	mks_out_buffer[2+ap_len] = key_len;
	memcpy((char *)&mks_out_buffer[2 + ap_len + 1], (const char *)MKS_WIFI_KEY, key_len);

	esp_frame.type=ESP_TYPE_NET;
	esp_frame.dataLen= 2 + ap_len + key_len + 1;
	esp_frame.data=mks_out_buffer;
	packet_size=mks_wifi_build_packet((uint8_t *)esp_packet,&esp_frame);

	if(packet_size > 8){ //4 байта заголовка + 2 байта длины + хвост + название сети и пароль
		//выпихнуть в uart
		mks_wifi_send((uint8_t *)esp_packet, packet_size);
	};
}

/*
Получает данные из всех функций, как только
есть перевод строки 0x0A, формирует пакет для
ESP и отправляет
*/
void mks_wifi_out_add(uint8_t *data, uint32_t size){
	static uint32_t line_index=0;
	uint32_t packet_size;
	ESP_PROTOC_FRAME esp_frame;

	if (espf_owns_uart()) return;     // QQS-Pro: the ESP is in its bootloader, Marlin's output would break the link

	while (size--){
		if(*data == 0x0a){
			//Перевод строки => сформировать пакет, отправить, сбросить индекс
			esp_frame.type=ESP_TYPE_FILE_FIRST; //Название типа из прошивки MKS. Смысла не имееет.
			esp_frame.dataLen=strnlen((char *)mks_out_buffer,MKS_OUT_BUFF_SIZE);
			esp_frame.data=mks_out_buffer;
			packet_size=mks_wifi_build_packet((uint8_t *)esp_packet,&esp_frame);

			if(packet_size == 0){
				ERROR("Build packet failed");
				line_index=0;
				memset(mks_out_buffer,0,MKS_OUT_BUFF_SIZE);
				break;
			}
			//выпихнуть в uart
			mks_wifi_send((uint8_t *)esp_packet, packet_size);
			qqs_wdiag.tx_lines++;
			qqs_diag_copy(qqs_wdiag.last_out, (const char *)mks_out_buffer);
			//очистить буфер
			memset(mks_out_buffer,0,MKS_OUT_BUFF_SIZE);
			//сбросить индекс
			line_index=0;
		}else{
			//писать в буфер			
			mks_out_buffer[line_index++]=*data++;
		}

		if(line_index >= MKS_OUT_BUFF_SIZE){
			ERROR("Max line size");
			line_index=0;
			memset(mks_out_buffer,0,MKS_OUT_BUFF_SIZE);
			break;
		}
	}
}

uint8_t mks_wifi_input(uint8_t data){
	ESP_PROTOC_FRAME esp_frame;
	#ifdef MKS_WIFI_ENABLED_WIFI_CONFIG 
	static uint8_t get_packet_from_esp=0;
	#endif
	static uint8_t packet_start_flag=0;
	static uint8_t packet_type=0;
	static uint16_t packet_index=0;
	static uint16_t payload_size=ESP_PACKET_DATA_MAX_SIZE;
	uint8_t ret_val=1;

	//Не отдавать данные в очередь команд, если идет печать
	// if (CardReader::isPrinting()){
	// 	DEBUG("No input while printing");
	// 	return 1;
	// }

	// QQS-Pro: 0xA5 inside a binary packet is data (network status: an IP octet of 165; scan list: UTF-8
	// names; upload start: file size bytes; any length byte). It restarts a packet only where the type
	// byte should be, or inside G-code text. A false start (stray 0xA5) is dropped at once by the type or
	// length check; a cut-off packet is caught by the tail check, which re-parses from the next 0xA5.
	// (No timeout: while the command queue is full Marlin stops reading, so a packet may arrive in parts.)
	if(data == ESP_PROTOC_HEAD && (!packet_start_flag || packet_index == 1 || (packet_index >= 4 && packet_type == ESP_TYPE_GCODE))){
		payload_size = ESP_PACKET_DATA_MAX_SIZE;
		packet_start_flag=1;
		packet_index=0;
		memset((uint8_t*)mks_in_buffer,0,MKS_IN_BUFF_SIZE);
	}else if(!packet_start_flag){
		qqs_wdiag.rx_stray++;
		DEBUG("Byte not in packet %0X - [%c]",data,data);
		return 1;
	}

	if(packet_start_flag){
		mks_in_buffer[packet_index]=data;
	}

	if(packet_index == 1){
		packet_type = mks_in_buffer[1];
		if(packet_type > 9){                 // QQS-Pro: not a protocol type -> not a real head
			qqs_wdiag.rx_stray++;
			packet_start_flag=0;
			packet_index=0;
			return 1;
		}
	}

	if(packet_index == 3){
		payload_size = uint16_t(mks_in_buffer[3] << 8) | mks_in_buffer[2];

		if(payload_size > ESP_PACKET_DATA_MAX_SIZE){
			ERROR("Payload size too big");
			packet_start_flag=0;
			packet_index=0;
			memset((uint8_t*)mks_in_buffer,0,MKS_IN_BUFF_SIZE);
			return 1;
		}

	}

	if( (packet_index >= (payload_size+4)) || (packet_index >= ESP_PACKET_DATA_MAX_SIZE) ){

		if(mks_wifi_check_packet((uint8_t *)mks_in_buffer)){
			qqs_wdiag.rx_bad++;
			ERROR("Packet check failed");
			packet_start_flag=0;
			// QQS-Pro: the packet was cut off and swallowed what followed; parse again from the next head
			static bool rescanning = false;
			static uint8_t rescan_buf[MKS_IN_BUFF_SIZE];
			if(!rescanning){
				uint16_t k = 1;
				while(k <= packet_index && mks_in_buffer[k] != ESP_PROTOC_HEAD) k++;
				const uint16_t n = (k <= packet_index) ? packet_index + 1 - k : 0;
				packet_index=0;
				if(n){
					memcpy(rescan_buf, (const uint8_t *)&mks_in_buffer[k], n);
					rescanning = true;
					for(uint16_t i = 0; i < n; i++) mks_wifi_input(rescan_buf[i]);
					rescanning = false;
				}
				return 1;   // a packet the re-parse started stays in progress
			}
			packet_index=0;
			return 1;
		}


		esp_frame.type = packet_type;
		esp_frame.dataLen = payload_size;
		esp_frame.data = (uint8_t*)&mks_in_buffer[4];

		mks_wifi_parse_packet(&esp_frame);

		#ifdef MKS_WIFI_ENABLED_WIFI_CONFIG 
		if(!get_packet_from_esp){
			DEBUG("Fisrt packet from ESP, send config");
		
			mks_wifi_set_param();
			get_packet_from_esp=1;
		}
		#endif
		packet_start_flag=0;
		packet_index=0;
	}

	if(packet_start_flag){
		packet_index++;
	}

	return ret_val;
}

/*
Проверяет, что символы из текстового диапазона
для G-code команд
*/
uint8_t check_char_allowed(char data){

	if( data == 0x0a || data == 0x0d){
		return 0;
	}

	if( (data >= 0x20) && (data <= 0x7E) ){
		return 0;
	}

	return 1;

}

/*
Проверяет пакет на корректность:
наличие заголовка
наличие "хвоста"
длина пакета
*/
uint8_t mks_wifi_check_packet(uint8_t *in_data){
	uint16_t payload_size;
	uint16_t tail_index;

	if(in_data[0] != ESP_PROTOC_HEAD){
		ERROR("Packet head mismatch");
		return 1;
	}

	payload_size = uint16_t(in_data[3] << 8) | in_data[2];
	
	if(payload_size > ESP_PACKET_DATA_MAX_SIZE){
		ERROR("Payload size mismatch");
		return 1;
	}
	
	tail_index = payload_size + 4;

	if(in_data[tail_index] != ESP_PROTOC_TAIL ){
		ERROR("Packet tail mismatch");
		return 1;
	}

	return 0;
}


uint8_t mks_wifi_ip[4] = {0,0,0,0};       // QQS-Pro UI: last reported IP
uint8_t mks_wifi_net_state = 0;            // 0 unknown, 1 connected, 2 exception, 3 not configured

void mks_wifi_parse_packet(ESP_PROTOC_FRAME *packet){
	static uint8_t show_ip_once=0;
	char str[100];

	if (packet->type < 5) qqs_wdiag.rx_pkt[packet->type]++; else qqs_wdiag.rx_other++;
	switch(packet->type){
		case ESP_TYPE_NET:
			qqs_net_parse(packet->data, packet->dataLen);
			memset(str,0,100);
			if(packet->data[6] == ESP_NET_WIFI_CONNECTED){
				memcpy(mks_wifi_ip, packet->data, 4);
				mks_wifi_net_state = 1;
			}else{
				mks_wifi_net_state = (packet->data[6] == ESP_NET_WIFI_EXCEPTION) ? 2 : 3;
			}
			if(packet->data[6] == ESP_NET_WIFI_CONNECTED){
				if(show_ip_once==0){
					show_ip_once=1;
					sprintf(str,"IP %d.%d.%d.%d",packet->data[0],packet->data[1],packet->data[2],packet->data[3]);
					ui.set_status((const char *)str,true);
					SERIAL_ECHO_START();
					SERIAL_ECHOLN((char*)str);	

					//Вывод имени сети
					
					if(packet->data[8] < 100){
						memcpy(str,&packet->data[9],packet->data[8]); 
						str[packet->data[8]]=0;
						SERIAL_ECHO_START();
						SERIAL_ECHO("WIFI: ");
						SERIAL_ECHOLN((char*)str);
					}else{
						DEBUG("Network NAME too long");
					}
				}
				DEBUG("[Net] connected, IP: %d.%d.%d.%d",packet->data[0],packet->data[1],packet->data[2],packet->data[3]);
			}else if(packet->data[6] == ESP_NET_WIFI_EXCEPTION){
				DEBUG("[Net] wifi exeption");
			}else{
				DEBUG("[Net] wifi not config");
			}
			break;
		case ESP_TYPE_GCODE:
				char gcode_cmd[50];
				uint32_t cmd_index;
				
				bool too_long;   // QQS-Pro: a line longer than the buffer is dropped instead of overflowing the stack
				too_long = false;
				cmd_index = 0;
				memset(gcode_cmd,0,50);
				for(uint32_t i=0; i<packet->dataLen; i++){
					
					if(packet->data[i] != 0x0A){
						if(too_long) continue;
						if(cmd_index >= sizeof(gcode_cmd)-1){ too_long = true; continue; }
						gcode_cmd[cmd_index++] = packet->data[i];
					}else{
						qqs_wdiag.last_in_i = (qqs_wdiag.last_in_i + 1) % 3;
						qqs_diag_copy(qqs_wdiag.last_in[qqs_wdiag.last_in_i], too_long ? "(too long, dropped)" : gcode_cmd);
						if(!too_long) GCodeQueue::ring_buffer.enqueue((const char *)gcode_cmd, false, MKS_WIFI_SERIAL_NUM);
						too_long = false;
						cmd_index = 0;
						memset(gcode_cmd,0,50);
					}
				}
			break;
		case ESP_TYPE_FILE_FIRST:
				DEBUG("[FILE_FIRST]");
				//Передача файла останавливает все процессы, 
				//поэтому печать в этот момент не возможна.
				if (!CardReader::isPrinting()){
					mks_wifi_start_file_upload(packet);
				}else{
					//Попытка запустить передачу файла во время печати
					//Отключение модуля wifi
					DEBUG("Shutdown wifi");
					WRITE(MKS_WIFI_IO_RST, LOW);
				}
			break;
		case ESP_TYPE_FILE_FRAGMENT:
				DEBUG("[FILE_FRAGMENT]");
			break;
		case ESP_TYPE_WIFI_LIST:
			DEBUG("[WIFI_LIST]");
			qqs_list_parse(packet->data, packet->dataLen);
			break;
		default:
			DEBUG("[Unkn]");
		 	break;

	}

}

uint16_t mks_wifi_build_packet(uint8_t *packet, ESP_PROTOC_FRAME *esp_frame){
	uint16_t packet_size=0;

	memset(packet,0,MKS_TOTAL_PACKET_SIZE);
	packet[0] = ESP_PROTOC_HEAD;
	packet[1] = esp_frame->type;

	for(uint32_t i=0; i < esp_frame->dataLen; i++){
		packet[i+4]=esp_frame->data[i]; //4 байта заголовка отступить
	}

	packet_size = esp_frame->dataLen + 4;

	if(packet_size > MKS_TOTAL_PACKET_SIZE){
		ERROR("ESP packet too big");
		return 0;
	}

	if(esp_frame->type != ESP_TYPE_NET){
		packet[packet_size++] = 0x0d;
		packet[packet_size++] = 0x0a; 
		esp_frame->dataLen = esp_frame->dataLen + 2; //Два байта на 0x0d 0x0a
	}
	
	*((uint16_t *)&packet[2]) = esp_frame->dataLen;

	packet[packet_size] = ESP_PROTOC_TAIL;
	return packet_size;
}


void mks_wifi_send(uint8_t *packet, uint16_t size){
	static uint32_t error_flag=0;
	uint32_t timeout;

	/* 
	Если уже был выход по превышению ожидания и место
	очереди так и не освободилось, сразу выход.
	*/

	if(error_flag == 1){
		if(MYSERIAL2.availableForWrite()==0){
			return;
		}else{
			error_flag = 0;
		}
	}
	
	UNUSED(timeout);
	for( uint32_t i=0; i < (uint32_t)(size+1); i++){
		const millis_t t0 = millis();
		while(MYSERIAL2.availableForWrite()==0){
			hal.watchdog_refresh();
			if(ELAPSED(millis(), t0 + 100)){
				qqs_wdiag.tx_timeouts++;
				error_flag=1;
				return;
			};
		}
		MYSERIAL2.write(packet[i]);
	}
}
#else
void mks_wifi_out_add(uint8_t *data, uint32_t size){
	while(size--){
		MYSERIAL2.write(*data++);
	}
	return;
};

#endif