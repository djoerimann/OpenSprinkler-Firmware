/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Weather functions
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler library
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>. 
 */

#include "OpenSprinkler.h"
#include "utils.h"
#include "server.h"
#include "weather.h"

extern OpenSprinkler os; // OpenSprinkler object
extern char tmp_buffer[];
extern char ether_buffer[];
char wt_rawData[TMP_BUFFER_SIZE];
int wt_errCode = 0;

byte findKeyVal (const char *str,char *strbuf, uint16_t maxlen,const char *key,bool key_in_pgm=false,uint8_t *keyfound=NULL);
void write_log(byte type, ulong curr_time);

// The weather function calls getweather.py on remote server to retrieve weather data
// the default script is WEATHER_SCRIPT_HOST/weather?.py
//static char website[] PROGMEM = DEFAULT_WEATHER_URL ;

static void getweather_callback(char* buffer) {
	char *p = buffer;
	DEBUG_PRINTLN(p);  
	/* scan the buffer until the first & symbol */
	while(*p && *p!='&') {
		p++;
	}
	if (*p != '&')	return;
	int v;
	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("sunrise"), true)) {
		v = atoi(tmp_buffer);
		if (v>=0 && v<=1440 && v != os.nvdata.sunrise_time) {
			os.nvdata.sunrise_time = v;
			os.nvdata_save();
			os.weather_update_flag |= WEATHER_UPDATE_SUNRISE;
		}
	}

	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("sunset"), true)) {
		v = atoi(tmp_buffer);
		if (v>=0 && v<=1440 && v != os.nvdata.sunset_time) {
			os.nvdata.sunset_time = v;
			os.nvdata_save();
			os.weather_update_flag |= WEATHER_UPDATE_SUNSET;			
		}
	}

	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("eip"), true)) {
		uint32_t l = atol(tmp_buffer);
		if(l != os.nvdata.external_ip) {
			os.nvdata.external_ip = atol(tmp_buffer);
			os.nvdata_save();
			os.weather_update_flag |= WEATHER_UPDATE_EIP;
		}
	}

	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("scale"), true)) {
		v = atoi(tmp_buffer);
		if (v>=0 && v<=250 && v != os.iopts[IOPT_WATER_PERCENTAGE]) {
			// only save if the value has changed
			os.iopts[IOPT_WATER_PERCENTAGE] = v;
			os.iopts_save();
			os.weather_update_flag |= WEATHER_UPDATE_WL;			
		}
	}
	
	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("tz"), true)) {
		v = atoi(tmp_buffer);
		if (v>=0 && v<= 108) {
			if (v != os.iopts[IOPT_TIMEZONE]) {
				// if timezone changed, save change and force ntp sync
				os.iopts[IOPT_TIMEZONE] = v;
				os.iopts_save();
				os.weather_update_flag |= WEATHER_UPDATE_TZ;
			}
		}
	}
	
	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("rd"), true)) {
		v = atoi(tmp_buffer);
		if (v>0) {
			os.nvdata.rd_stop_time = os.now_tz() + (unsigned long) v * 3600;
			os.raindelay_start();
		} else if (v==0) {
			os.raindelay_stop();
		}
	}

	if (findKeyVal(p, wt_rawData, TMP_BUFFER_SIZE, PSTR("rawData"), true)) {
		wt_rawData[TMP_BUFFER_SIZE-1]=0;	// make sure the buffer ends properly
	} else {
		wt_rawData[0] = 0;
	}
	
	if (findKeyVal(p, tmp_buffer, TMP_BUFFER_SIZE, PSTR("errCode"), true)) {
		wt_errCode = atoi(tmp_buffer);
	} else {
		wt_errCode = 0;
	} 

	os.checkwt_success_lasttime = os.now_tz();
	write_log(LOGDATA_WATERLEVEL, os.checkwt_success_lasttime);
}

static void getweather_callback_with_peel_header(char* buffer) {
	peel_http_header(buffer);
	getweather_callback(buffer);
}

void GetWeather() {
#if defined(ESP8266)
	if(!m_server) {
		if (os.state!=OS_STATE_CONNECTED || WiFi.status()!=WL_CONNECTED) return;
	}
#endif
	// use temp buffer to construct get command
	BufferFiller bf = tmp_buffer;
	bf.emit_p(PSTR("$D?loc=$O&wto=$O&fwv=$D"),
								(int) os.iopts[IOPT_USE_WEATHER],
								SOPT_LOCATION,
								SOPT_WEATHER_OPTS,
								(int)os.iopts[IOPT_FW_VERSION]);

	char *src=tmp_buffer+strlen(tmp_buffer);
	char *dst=tmp_buffer+TMP_BUFFER_SIZE-12;
	
	char c;
	// url encode. convert SPACE to %20
	// copy reversely from the end because we are potentially expanding
	// the string size 
	while(src!=tmp_buffer) {
		c = *src--;
		if(c==' ') {
			*dst-- = '0';
			*dst-- = '2';
			*dst-- = '%';
		} else {
			*dst-- = c;
		}
	};
	*dst = *src;

	strcpy(ether_buffer, "GET /");
	strcat(ether_buffer, dst);
	// because dst is part of tmp_buffer,
	// must load weather url AFTER dst is copied to ether_buffer	

	// load weather url to tmp_buffer
	char *host = tmp_buffer;
	os.sopt_load(SOPT_WEATHERURL, host);

	strcat(ether_buffer, " HTTP/1.0\r\nHOST: ");
	strcat(ether_buffer, host);
	strcat(ether_buffer, "\r\n\r\n");

	DEBUG_PRINTLN(ether_buffer);
	
	os.send_http_request(host, ether_buffer, getweather_callback_with_peel_header);
}

