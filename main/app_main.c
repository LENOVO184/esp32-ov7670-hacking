// Kopyright tekker 2017 - ESPILICAM - based on original esp32-cam code by igrr
// with additions for ov7670 sensor, ili9341 lcd display, telnet command module
// .bmp file generation, 32-bit aligned framebuffer, graphic overlay support
// and some other cool things
// modified telnet lib by n.kolban et al
// modified smallargs lib by Fabian Meyer
// modified ov7670 code based on openmn and freescale libs
// with thanks to ataweg for segmented framebuffer
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <byteswap.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "soc/spi_reg.h"
#include "driver/hspi.h"
#include "soc/gpio_reg.h"
#include "esp_attr.h"

#include "soc/gpio_struct.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "camera.h"
#include "framebuffer.h"

#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "bitmap.h"

#include "telnet.h"
#include "graphics.h"

static const char* TAG = "ESPILICAM";

/*
 This code displays some fancy graphics on the ILI9341-based 320x240 LCD on an ESP-WROVER_KIT board.
 It is not very fast, even when the SPI transfer itself happens at 8MHz and with DMA, because
 the rest of the code is not very optimized. Especially calculating the image line-by-line
 is inefficient; it would be quicker to send an entire screenful at once. This example does, however,
 demonstrate the use of both spi_device_transmit as well as spi_device_queue_trans/spi_device_get_trans_result
 as well as pre-transmit callbacks.

 Some info about the ILI9341: It has an C/D line, which is connected to a GPIO here. It expects this
 line to be low for a command and high for data. We use a pre-transmit callback here to control that
 line: every transaction has as the user-definable argument the needed state of the D/C line and just
 before the transaction is sent, the callback will set this line to the correct state.
 */

#define PIN_NUM_MISO CONFIG_HW_LCD_MISO_GPIO
#define PIN_NUM_MOSI CONFIG_HW_LCD_MOSI_GPIO
#define PIN_NUM_CLK  CONFIG_HW_LCD_CLK_GPIO
#define PIN_NUM_CS   CONFIG_HW_LCD_CS_GPIO
#define PIN_NUM_DC   CONFIG_HW_LCD_DC_GPIO
#define PIN_NUM_RST  CONFIG_HW_LCD_RESET_GPIO
#define PIN_NUM_BCKL CONFIG_HW_LCD_BL_GPIO

SemaphoreHandle_t dispSem = NULL;
SemaphoreHandle_t dispDoneSem = NULL;

/*
 The ILI9341 needs a bunch of command/argument values to be initialized. They are stored in this struct.
 */
typedef struct {
	uint8_t cmd;
	uint8_t data[16];
	uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} ili_init_cmd_t;

static spi_device_handle_t spi;

//Place data into DRAM. Constant data gets placed into DROM by default, which is not accessible by DMA.
DRAM_ATTR static const ili_init_cmd_t ili_init_cmds[] = { { 0xCF, { 0x00, 0x83,
		0X30 }, 3 }, { 0xED, { 0x64, 0x03, 0X12, 0X81 }, 4 }, { 0xE8, { 0x85,
		0x01, 0x79 }, 3 }, { 0xCB, { 0x39, 0x2C, 0x00, 0x34, 0x02 }, 5 }, {
		0xF7, { 0x20 }, 1 }, { 0xEA, { 0x00, 0x00 }, 2 }, { 0xC0, { 0x26 }, 1 },
		{ 0xC1, { 0x11 }, 1 }, { 0xC5, { 0x35, 0x3E }, 2 },
		{ 0xC7, { 0xBE }, 1 }, { 0x36, { 0x28 }, 1 }, { 0x3A, { 0x55 }, 1 }, {
				0xB1, { 0x00, 0x1B }, 2 }, { 0xF2, { 0x08 }, 1 }, { 0x26,
				{ 0x01 }, 1 }, { 0xE0, { 0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06,
				0x45, 0X87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00 }, 15 }, {
				0XE1, { 0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D,
						0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F }, 15 }, { 0x2A, {
				0x00, 0x00, 0x00, 0xEF }, 4 }, { 0x2B,
				{ 0x00, 0x00, 0x01, 0x3f }, 4 }, { 0x2C, { 0 }, 0 }, { 0xB7, {
				0x07 }, 1 }, { 0xB6, { 0x0A, 0x82, 0x27, 0x00 }, 4 }, { 0x11, {
				0 }, 0x80 }, { 0x29, { 0 }, 0x80 }, { 0, { 0 }, 0xff }, };

//Send a command to the ILI9341. Uses spi_device_transmit, which waits until the transfer is complete.
void ili_cmd(spi_device_handle_t spi, const uint8_t cmd) {
	esp_err_t ret;
	spi_transaction_t t;
	memset(&t, 0, sizeof(t));       //Zero out the transaction
	t.length = 8;                     //Command is 8 bits
	t.tx_buffer = &cmd;               //The data is the cmd itself
	t.user = (void*) 0;                //D/C needs to be set to 0
	ret = spi_device_transmit(spi, &t);  //Transmit!
	assert(ret==ESP_OK);            //Should have had no issues.
}

//Send data to the ILI9341. Uses spi_device_transmit, which waits until the transfer is complete.
void ili_data(spi_device_handle_t spi, const uint8_t *data, int len) {
	esp_err_t ret;
	spi_transaction_t t;
	if (len == 0)
		return;             //no need to send anything
	memset(&t, 0, sizeof(t));       //Zero out the transaction
	t.length = len * 8;        //Len is in bytes, transaction length is in bits.
	t.tx_buffer = data;               //Data
	t.user = (void*) 1;                //D/C needs to be set to 1
	ret = spi_device_transmit(spi, &t);  //Transmit!
	assert(ret==ESP_OK);            //Should have had no issues.
}

//This function is called (in irq context!) just before a transmission starts. It will
//set the D/C line to the value indicated in the user field.
void ili_spi_pre_transfer_callback(spi_transaction_t *t) {
	int dc = (int) t->user;
	gpio_set_level(PIN_NUM_DC, dc);
}

//Initialize the display
void ili_init(spi_device_handle_t spi) {
	int cmd = 0;
	//Initialize non-SPI GPIOs
	gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
	gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
	gpio_set_direction(PIN_NUM_BCKL, GPIO_MODE_OUTPUT);
	//Reset the display
	gpio_set_level(PIN_NUM_RST, 0);
	vTaskDelay(100 / portTICK_RATE_MS);
	gpio_set_level(PIN_NUM_RST, 1);
	vTaskDelay(100 / portTICK_RATE_MS);
	//Send all the commands
	while (ili_init_cmds[cmd].databytes != 0xff) {
		ili_cmd(spi, ili_init_cmds[cmd].cmd);
		ili_data(spi, ili_init_cmds[cmd].data,
				ili_init_cmds[cmd].databytes & 0x1F);
		if (ili_init_cmds[cmd].databytes & 0x80) {
			vTaskDelay(100 / portTICK_RATE_MS);
		}
		cmd++;
	}
	///Enable backlight
	gpio_set_level(PIN_NUM_BCKL, 1);
}

//To send a line we have to send a command, 2 data bytes, another command, 2 more data bytes and another command
//before sending the line data itself; a total of 6 transactions. (We can't put all of this in just one transaction
//because the D/C line needs to be toggled in the middle.)
//This routine queues these commands up so they get sent as quickly as possible.
static void send_line(spi_device_handle_t spi, int ypos, uint16_t *line) {
	esp_err_t ret;
	int x;
	//Transaction descriptors. Declared static so they're not allocated on the stack; we need this memory even when this
	//function is finished because the SPI driver needs access to it even while we're already calculating the next line.
	static spi_transaction_t trans[6];

	//In theory, it's better to initialize trans and data only once and hang on to the initialized
	//variables. We allocate them on the stack, so we need to re-init them each call.
	for (x = 0; x < 6; x++) {
		memset(&trans[x], 0, sizeof(spi_transaction_t));
		if ((x & 1) == 0) {
			//Even transfers are commands
			trans[x].length = 8;
			trans[x].user = (void*) 0;
		} else {
			//Odd transfers are data
			trans[x].length = 8 * 4;
			trans[x].user = (void*) 1;
		}
		trans[x].flags = SPI_TRANS_USE_TXDATA;
	}
	trans[0].tx_data[0] = 0x2A;           //Column Address Set
	trans[1].tx_data[0] = 0;              //Start Col High
	trans[1].tx_data[1] = 0;              //Start Col Low
	trans[1].tx_data[2] = (320) >> 8;       //End Col High
	trans[1].tx_data[3] = (320) & 0xff;     //End Col Low
	trans[2].tx_data[0] = 0x2B;           //Page address set
	trans[3].tx_data[0] = ypos >> 8;        //Start page high
	trans[3].tx_data[1] = ypos & 0xff;      //start page low
	trans[3].tx_data[2] = (ypos + 1) >> 8;    //end page high
	trans[3].tx_data[3] = (ypos + 1) & 0xff;  //end page low
	trans[4].tx_data[0] = 0x2C;           //memory write
	trans[5].tx_buffer = line;            //finally send the line data
	trans[5].length = 320 * 2 * 8;            //Data length, in bits
	trans[5].flags = 0; //undo SPI_TRANS_USE_TXDATA flag

	//Queue all transactions.
	for (x = 0; x < 6; x++) {
		ret = spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
		assert(ret==ESP_OK);
	}

	//When we are here, the SPI driver is busy (in the background) getting the transactions sent. That happens
	//mostly using DMA, so the CPU doesn't have much to do here. We're not going to wait for the transaction to
	//finish because we may as well spend the time calculating the next line. When that is done, we can call
	//send_line_finish, which will wait for the transfers to be done and check their status.
}

static void send_line_finish(spi_device_handle_t spi) {
	spi_transaction_t *rtrans;
	esp_err_t ret;
	//Wait for all 6 transactions to be done and get back the results.
	for (int x = 0; x < 6; x++) {
		ret = spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
		assert(ret==ESP_OK);
		//We could inspect rtrans now if we received any info back. The LCD is treated as write-only, though.
	}
}

void spi_lcd_wait_finish() {
	xSemaphoreTake(dispDoneSem, portMAX_DELAY);
}

void spi_lcd_send() {
	xSemaphoreGive(dispSem);
}

SemaphoreHandle_t captureDoneSem = NULL;
SemaphoreHandle_t captureSem = NULL;

void capture_wait_finish() {
	xSemaphoreTake(captureDoneSem, portMAX_DELAY);
}

void capture_request() {
	xSemaphoreGive(captureSem);
}

static EventGroupHandle_t espilicam_event_group;
EventBits_t uxBits;
const int MOVIEMODE_ON_BIT = BIT0;
const int OVERLAY_ON_BIT = BIT1;
const int STREAMING_ON_BIT = BIT2;

bool is_moviemode_on() {
	return (xEventGroupGetBits(espilicam_event_group) & MOVIEMODE_ON_BIT) ?
			1 : 0;
}

static void set_moviemode(bool c) {
	if (is_moviemode_on() == c) {
		return;
	} else {
		if (c) {
			xEventGroupSetBits(espilicam_event_group, MOVIEMODE_ON_BIT);
		} else {
			xEventGroupClearBits(espilicam_event_group, MOVIEMODE_ON_BIT);
		}
	}
}

bool is_overlay_on() {
	return (xEventGroupGetBits(espilicam_event_group) & OVERLAY_ON_BIT) ?
			1 : 0;
}

static void set_overlay(bool c) {
	if (is_overlay_on() == c) {
		return;
	} else {
		if (c) {
			xEventGroupSetBits(espilicam_event_group, OVERLAY_ON_BIT);
		} else {
			xEventGroupClearBits(espilicam_event_group, OVERLAY_ON_BIT);
		}
	}
}

bool is_streaming_on() {
	return (xEventGroupGetBits(espilicam_event_group) & STREAMING_ON_BIT) ?
			1 : 0;
}

static void set_streaming(bool c) {
	if (is_streaming_on() == c) {
		return;
	} else {
		if (c) {
			xEventGroupSetBits(espilicam_event_group, STREAMING_ON_BIT);
		} else {
			xEventGroupClearBits(espilicam_event_group, STREAMING_ON_BIT);
		}
	}
}

// TODO: add bool blocking? var. to replace spi_lcd_send / spi_lcd_wait_finish func..
static void request_display_refresh() {
	// if inside moviemode, display will refresh continually after every capture...
	if (!is_moviemode_on())
	  xSemaphoreGive(dispSem);
}

// adding a ms delay to lower frame rate OR setup mutex/lock for FB write /read
static uint16_t lcd_delay_ms = 100;

// TODO: refactor capture/display logic
static void captureTask(void *pvParameters) {

	bool movie_mode = false;

	while (1) {
		//frame++;
		movie_mode = is_moviemode_on();
		if (!movie_mode)
			xSemaphoreTake(captureSem, portMAX_DELAY);

		camera_run();

		spi_lcd_send(); // sem give dispSem
		spi_lcd_wait_finish(); // sem take dispDoneSem

		if (!movie_mode)
			xSemaphoreGive(captureDoneSem);
		// only return when LCD finished display .. sort of..
	} // end while(1)

}

static int debugDisplay = 0;

#define ILI_WIDTH 320
#define ILI_HEIGHT 240

// CAMERA CONFIG
#ifdef CONFIG_ENABLE_TEST_PATTERN
#define ENABLE_TEST_PATTERN 1
#else
#define ENABLE_TEST_PATTERN 0
#endif

static camera_pixelformat_t s_pixel_format;
static camera_config_t config = { .ledc_channel = LEDC_CHANNEL_0, .ledc_timer =
		LEDC_TIMER_0, .pin_d0 = CONFIG_D0, .pin_d1 = CONFIG_D1, .pin_d2 =
		CONFIG_D2, .pin_d3 = CONFIG_D3, .pin_d4 = CONFIG_D4,
		.pin_d5 = CONFIG_D5, .pin_d6 = CONFIG_D6, .pin_d7 = CONFIG_D7,
		.pin_xclk = CONFIG_XCLK, .pin_pclk = CONFIG_PCLK, .pin_vsync =
				CONFIG_VSYNC, .pin_href = CONFIG_HREF, .pin_sscb_sda =
				CONFIG_SDA, .pin_sscb_scl = CONFIG_SCL, .pin_reset =
				CONFIG_RESET, .xclk_freq_hz = CONFIG_XCLK_FREQ,
		.test_pattern_enabled = ENABLE_TEST_PATTERN, };

static camera_model_t camera_model;

#define CAMERA_PIXEL_FORMAT CAMERA_PF_RGB565
//#define CAMERA_PIXEL_FORMAT CAMERA_PF_YUV422
#define CAMERA_FRAME_SIZE CAMERA_FS_QVGA

// DISPLAY LOGIC
static inline uint8_t clamp(int n) {
	n = n > 255 ? 255 : n;
	return n < 0 ? 0 : n;
}

// Pass 8-bit (each) R,G,B, get back 16-bit packed color
static inline uint16_t ILI9341_color565(uint8_t r, uint8_t g, uint8_t b) {
	return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t get_grayscale_pixel_as_565(uint8_t pix) {
	// R = (img[n]&248)<<8; // 5 bit cao cua Y
	// G = (img[n]&252)<<3; // 6 bit cao cua Y
	// B = (img[n]&248)>>3; // 5 bit cao cua Y
	uint16_t graypixel = ((pix & 248) << 8) | ((pix & 252) << 3)
			| ((pix & 248) >> 3);
	return graypixel;

}

// integers instead of floating point...
static inline uint16_t fast_yuv_to_rgb565(int y, int u, int v) {
	int a0 = 1192 * (y - 16);
	int a1 = 1634 * (v - 128);
	int a2 = 832 * (v - 128);
	int a3 = 400 * (u - 128);
	int a4 = 2066 * (u - 128);
	int r = (a0 + a1) >> 10;
	int g = (a0 - a2 - a3) >> 10;
	int b = (a0 + a4) >> 10;
	return ILI9341_color565(clamp(r), clamp(g), clamp(b));

}

// fast but uses floating points...
static inline uint16_t fast_pascal_to_565(int Y, int U, int V) {
	uint8_t r, g, b;
	r = clamp(1.164 * (Y - 16) + 1.596 * (V - 128));
	g = clamp(1.164 * (Y - 16) - 0.392 * (U - 128) - 0.813 * (V - 128));
	b = clamp(1.164 * (Y - 16) + 2.017 * (U - 128));
	return ILI9341_color565(r, g, b);
}

inline uint8_t unpack(int byteNumber, uint32_t value) {
	return (value >> (byteNumber * 8));
}

static void render_overlay_to_fb(char* fpsString) {
	// lock framebuffer if needed...
	drawOverlayGraphics(fpsString);
}

// from esp32-hal..
/*
unsigned long IRAM_ATTR millis()
{
      return xTaskGetTickCount() * portTICK_PERIOD_MS;

}
*/

#define millis() (xTaskGetTickCount()*portTICK_PERIOD_MS)

#define FPS_CALC_INTERVAL_MS 250


static void push_framebuffer_to_tft(void *pvParameters) {
	uint16_t line[2][320];
	static char fpsString[15] = "STARTING";
	int x, y; //, frame=0;
	//Indexes of the line currently being sent to the LCD and the line we're calculating.
	int sending_line = -1;
	int calc_line = 0;

	uint32_t* fbl;    // address of current line/row in framebuffer
	fb_context_t fbc_display;

	int ili_width = 320;
	int ili_height = 240;
	int width = camera_get_fb_width();
	int height = camera_get_fb_height();
	uint16_t pixel565 = 0;
	uint16_t pixel565_2 = 0;
	int current_byte_pos = 0;
	int current_pixel_pos = 0;
	int current_line_pos = 0;

	unsigned long now = millis();
	unsigned long lastcalc = 0;
	unsigned long framedispcnt = 0;
	unsigned long calcelapsed = 0;
	double fps = 0;

	while (1) {
		xSemaphoreTake(dispSem, portMAX_DELAY);

		// TODO: mutex as needed or move render call outside of push_framebuffer task
		if (is_overlay_on()) {
			  render_overlay_to_fb(fpsString);
		}

		//
		for (y = 0; y < ili_height; y++) {
			// begin of line/row
			current_pixel_pos = (y % height) * width;

			fbl = (uint32_t*) framebuffer_pos_32(&fbc_display,
					current_pixel_pos);

			//Calculate a line, operate on 2 pixels at a time...
			for (x = 0; x < ili_width; x += 2) {
				// wrap pixels around...
				current_line_pos = x % width;
				current_byte_pos = current_line_pos / 2;

				if (fbl != NULL) {
					if (s_pixel_format == CAMERA_PF_YUV422) {
						uint32_t long2px = 0;
						uint8_t y1, y2, u, v;

						long2px = fbl[current_byte_pos];
						y1 = unpack(0, long2px);
						u = unpack(1, long2px);
						;
						y2 = unpack(2, long2px);
						v = unpack(3, long2px);

						pixel565 = fast_yuv_to_rgb565(y1, u, v);
						pixel565_2 = fast_yuv_to_rgb565(y2, u, v);
						// swap bytes for ILI
						line[calc_line][x] = __bswap_16(pixel565);
						line[calc_line][x + 1] = __bswap_16(pixel565_2);
					} else {
						// rgb565 direct from OV7670 to ILI9341
						// best to swap bytes here instead of bswap
						uint32_t long2px = 0;
						long2px = fbl[current_byte_pos];
						pixel565 = (unpack(0, long2px) << 8)
								| unpack(1, long2px);
						pixel565_2 = (unpack(2, long2px) << 8)
								| unpack(3, long2px);

						line[calc_line][x] = __bswap_16(pixel565);
						line[calc_line][x + 1] = __bswap_16(pixel565_2);
					}
				}
			}
			//Finish up the sending process of the previous line, if any
			if (sending_line != -1)
				send_line_finish(spi);
			//Swap sending_line and calc_line
			sending_line = calc_line;
			calc_line = (calc_line == 1) ? 0 : 1;
			//Send the line we currently calculated.
			send_line(spi, y, line[sending_line]);
			//The line is queued up for sending now; the actual sending happens in the
			//background. We can go on to calculate the next line as long as we do not
			//touch line[sending_line]; the SPI sending process is still reading from that.
		} // end for (y=0; y<ili_height; y++)

		// check that line is actually sent before giving semaphore!
		if (sending_line != -1)
			send_line_finish(spi);
		sending_line = -1;

		now =  millis();
		calcelapsed = (now - lastcalc);
		if ((now - lastcalc) >= FPS_CALC_INTERVAL_MS) {
					fps = (double)framedispcnt * (double)1000 / (double)calcelapsed;
					sprintf(fpsString, "%4.2f FPS", fps);
					framedispcnt = 0;
					lastcalc = now;
		}
		framedispcnt++;
		xSemaphoreGive(dispDoneSem);
		// frame display finished
	} // end while(1)
}

// camera code

const static char http_hdr[] = "HTTP/1.1 200 OK\r\n";
const static char http_stream_hdr[] =
		"Content-type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n\r\n";
const static char http_jpg_hdr[] = "Content-type: image/jpg\r\n\r\n";
const static char http_pgm_hdr[] =
		"Content-type: image/x-portable-graymap\r\n\r\n";
const static char http_stream_boundary[] =
		"--123456789000000000000987654321\r\n";
const static char http_bitmap_hdr[] = "Content-type: image/bitmap\r\n\r\n";
const static char http_yuv422_hdr[] =
		"Content-Disposition: attachment; Content-type: application/octet-stream\r\n\r\n";

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
static ip4_addr_t s_ip_addr;

// command parser...
#include "smallargs.h"

#define UNUSED(x) ((void)x)
#define RESPONSE_BUFFER_LEN 256
#define CMD_BUFFER_LEN 128

static sarg_root root;
static char telnet_cmd_response_buff[RESPONSE_BUFFER_LEN];
static char telnet_cmd_buffer[CMD_BUFFER_LEN];

static void handle_camera_config_chg(bool reinit_reqd) {
	if (reinit_reqd) {
		ESP_LOGD(TAG, "Reconfiguring camera...");
		esp_err_t err;
		vTaskDelay(100 / portTICK_RATE_MS);
		err = reset_pixformat();
		config.pixel_format = s_pixel_format;
		err = camera_init(&config);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
			//return;
		}
		return;
		vTaskDelay(100 / portTICK_RATE_MS);
	}
}

static int help_cb(const sarg_result *res) {
	UNUSED(res);
	char *buf;
	int ret;

	ret = sarg_help_text(&root, &buf);
	if (ret != SARG_ERR_SUCCESS)
		return ret;
	telnet_esp32_sendData((uint8_t *) buf, strlen(buf));
	free(buf);
	return 0;
}

static int convertCSVCharArrayToIntArray(char* string, int* output,
		int maxElements) {
	//const char string[] = "comma separated,input,,,some fields,,empty";
	const char delims[] = ",";
	int elementCnt = 0;
	int n = 0;

	// const char *s = string;
	// if we don't want to eat the string and dont want to malloc we can use this monstrosity
	#define BUF_CHARS_TMP 50
	static char tmpStr[BUF_CHARS_TMP];
	strncpy(tmpStr, string, BUF_CHARS_TMP);
	tmpStr[sizeof(tmpStr) - 1] = 0;
	const char *s = &tmpStr[0];

	do {
		if (elementCnt >= maxElements)
			break;
		size_t field_len = strcspn(s, delims);
		//printf("\"%.*s\" - %d\n", (int)field_len, s, (int)field_len);
		if (field_len > 0) {
			n = atoi(s);
			output[elementCnt++] = n;
		}
		s += field_len;
	} while (*s++);
	return elementCnt;

}

// modified version of stats command for debugging
// this code to be removed if not needed...
static int sys_stats_cb(const sarg_result *res) {

	uint8_t level = 0;
	size_t free8start = 0, free32start = 0, free8 = 0, free32 = 0, tstk = 0;
	uint8_t length = 0;
	char *s_args = res->str_val;

#define SSCB_MAX_ARGS 10
	int argsArray[SSCB_MAX_ARGS];
	int els = convertCSVCharArrayToIntArray(s_args, argsArray, SSCB_MAX_ARGS);
	if (els > 0) {

		if (els == 1) {
			if (argsArray[0] == 0) {

				free32 = heap_caps_get_largest_free_block( MALLOC_CAP_32BIT);
				free8 = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT);
				free8start = heap_caps_get_minimum_free_size( MALLOC_CAP_8BIT);
				free32start = heap_caps_get_minimum_free_size(
						MALLOC_CAP_32BIT);

				tstk = uxTaskGetStackHighWaterMark(NULL);
				length +=
						sprintf(telnet_cmd_response_buff + length,
								"Stack: %db, free 8-bit=%db, free 32-bit=%db, min 8-bit=%db, min 32-bit=%db.\n",
								tstk, free8, free32, free8start, free32start);

			}
			if (argsArray[0] == 1) {
				displayHelpScreen();
				request_display_refresh();
				//xSemaphoreGive(dispSem);
			}
		}
		if (els == 2) {
			// handle 1,0 - debugdisplay on, off
			if (argsArray[0] == 1) {
				if (argsArray[1] == 0)
					debugDisplay = false;
				else
					debugDisplay = true;
			}
			// handle 2,0
			if (argsArray[0] == 2) {
				bool overlay = false;
				bool curr_overlay = is_overlay_on();
				if (argsArray[1] == 0)
					overlay = false;
				else
					overlay = true;

				if (overlay != curr_overlay) {
					set_overlay(overlay);
					request_display_refresh();
					length += sprintf(telnet_cmd_response_buff + length,
									"set overlay to %d\n", overlay);
				}

			}
		}
		if (els >= 6) {
			// handle 2,100,150,200,200,4444 etc. for fillRect
			if (argsArray[0] == 2) {
				fillRect(argsArray[1], argsArray[2], argsArray[3], argsArray[4],
						argsArray[5]);
				//xSemaphoreGive(dispSem);
				request_display_refresh();
			}
		}
		length += sprintf(telnet_cmd_response_buff + length,
				"fillrect called for %dx%d\n", level, level);
	}
	telnet_esp32_sendData((uint8_t *) telnet_cmd_response_buff,
			strlen(telnet_cmd_response_buff));
	return SARG_ERR_SUCCESS;
}

static int videomode_cb(const sarg_result *res) {
	int bval = 0;
	uint8_t length = 0;
	bval = res->int_val;
	bool movie_mode = false;
	bool curr_movie_mode = is_moviemode_on();
	// let capture task handle this...
	if (bval == 0)
		movie_mode = false;
	else if (bval == 1)
		movie_mode = true;
	else {
		(lcd_delay_ms = res->int_val);
		movie_mode = true;
	}
	// set event group...
	if (movie_mode != curr_movie_mode) {
		set_moviemode(movie_mode);
		if (movie_mode) {
			// start capture task!
			length += sprintf(telnet_cmd_response_buff + length,
					"video mode on\n");
			capture_request();
		} else {
			capture_wait_finish();
			length += sprintf(telnet_cmd_response_buff + length,
					"video mode off\n");
		}
	} else {
		length += sprintf(telnet_cmd_response_buff + length,
				"video mode unchanged\n");
	}
	telnet_esp32_sendData((uint8_t *) telnet_cmd_response_buff,
			strlen(telnet_cmd_response_buff));
	return SARG_ERR_SUCCESS;
}

// handle boolean value 1-0
static int overlay_cb(const sarg_result *res) {
	uint8_t length = 0;
	bool setting_req_val = false;
	if ((res->int_val) != 0) setting_req_val = true;
	bool setting_curr_val = is_overlay_on();
	static const char *settingName = "overlay";
	static char settingValTmp[10];
	if (setting_curr_val != setting_req_val) {
		set_overlay(setting_req_val); // actual setting to change
		if (setting_req_val)
			sprintf(settingValTmp,"on");
		else
			sprintf(settingValTmp,"off");
	} else {
		sprintf(settingValTmp,"unchanged");
	}
	length += sprintf(telnet_cmd_response_buff + length, "%s %s.\n",settingName,settingValTmp);

/*
	bool overlay = false;
	bool curr_overlay = is_overlay_on();
	if (bval == 0)
		overlay = false;
	else {
		overlay = true;
	}
	// set event group...
	if (overlay != curr_overlay) {
		set_overlay(overlay);
		if (overlay) {
			length += sprintf(telnet_cmd_response_buff + length,
					"overlay on\n");
		} else {
			length += sprintf(telnet_cmd_response_buff + length,
					"overlay off\n");
		}
	} else {
		length += sprintf(telnet_cmd_response_buff + length,
				"overlay unchanged\n");
	}
	telnet_esp32_sendData((uint8_t *) telnet_cmd_response_buff,
			strlen(telnet_cmd_response_buff));


	*/
	return SARG_ERR_SUCCESS;
}


static int ov7670_xclck_cb(const sarg_result *res) {
	int speed = 0;
	speed = res->int_val;
	ESP_LOGD(TAG, "Switch XCLCK to %dMHZ", speed);
	speed = speed * 1000000;
	config.xclk_freq_hz = speed;
	reset_xclk(&config);
	handle_camera_config_chg(true);
	return SARG_ERR_SUCCESS;
}

static int ov7670_pixformat_cb(const sarg_result *res) {
	if (strcmp("yuv422", res->str_val) == 0) {
		//
		ESP_LOGD(TAG, "Switch pixel format to YUV422");
		s_pixel_format = CAMERA_PF_YUV422;
		handle_camera_config_chg(true);
	} else if (strcmp("rgb565", res->str_val) == 0) {
		//
		ESP_LOGD(TAG, "Switch pixel format to RGB565");
		s_pixel_format = CAMERA_PF_RGB565;
		handle_camera_config_chg(true);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_framerate_cb(const sarg_result *res) {
	int framerate = 0;
	framerate = res->int_val;
	ESP_LOGD(TAG, "Switch framerate to %dFPS", framerate);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		// this framerate parameter is a hack, openmv params are different
		if (framerate == 14)
			s_sensor->set_framerate(s_sensor, 0);
		if (framerate == 15)
			s_sensor->set_framerate(s_sensor, 1);
		if (framerate == 25)
			s_sensor->set_framerate(s_sensor, 2);
		if (framerate == 30)
			s_sensor->set_framerate(s_sensor, 3);
		//  handle_camera_config_chg(true);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_colorbar_cb(const sarg_result *res) {

	bool onoff = false;
	if (res->int_val == 1)
		onoff = true;
	config.test_pattern_enabled = onoff;
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		ESP_LOGD(TAG, "Set Colorbar (Test Pattern) %d",
				config.test_pattern_enabled);
		s_sensor->set_colorbar(s_sensor, config.test_pattern_enabled);
	}
	handle_camera_config_chg(false);
	return SARG_ERR_SUCCESS;
}

static int ov7670_saturation_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch saturation (0-256) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_saturation(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_hue_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch saturation (-180 to 180) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_hue(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_brightness_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch brightness (-4 to 4) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_brightness(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_contrast_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch contrast (-4 to 4) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_contrast(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_hflip_cb(const sarg_result *res) {
	//set_vflip
	bool onoff = false;
	if (res->int_val == 1)
		onoff = true;
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		ESP_LOGD(TAG, "Set hflip = %d", onoff);
		s_sensor->set_hmirror(s_sensor, onoff);
	}
	//handle_camera_config_chg(false);
	return SARG_ERR_SUCCESS;
}

static int ov7670_vflip_cb(const sarg_result *res) {
	bool onoff = false;
	if (res->int_val == 1)
		onoff = true;
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		ESP_LOGD(TAG, "Set vflip = %d", onoff);
		s_sensor->set_vflip(s_sensor, onoff);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_lightmode_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch lightmode (0 - 5) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_ov7670_light_mode(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_nightmode_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch nightmode effect (0 - 3) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_ov7670_night_mode(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_special_effects_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch special effect (0 - 8) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_special_effect(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_gamma_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch gamma (0 - 1) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_ov7670_gamma(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

static int ov7670_whitebalance_cb(const sarg_result *res) {
	int level = 0;
	level = res->int_val;
	ESP_LOGD(TAG, "Switch whitebalance effect (0 - 2) to %d", level);
	sensor_t* s_sensor = get_cam_sensor();
	if (s_sensor != NULL) {
		s_sensor->set_ov7670_whitebalance(s_sensor, level);
	}
	return SARG_ERR_SUCCESS;
}

const static sarg_opt my_opts[] = { { "h", "help", "show help text", BOOL,
		help_cb }, { "s", "stats", "system stats (0=mem,1=tasks)", STRING,
		sys_stats_cb },
//    {"s", "stats", "system stats (0=mem,1=tasks)", INT, sys_stats_cb},
		{ NULL, "clock", "set camera xclock frequency", INT, ov7670_xclck_cb },
		{ NULL, "pixformat", "set pixel format (yuv422, rgb565)", STRING,
				ov7670_pixformat_cb }, { NULL, "framerate",
				"set framerate (14,15,25,30)", INT, ov7670_framerate_cb }, {
				NULL, "colorbar", "set test pattern (0=off/1=on)", INT,
				ov7670_colorbar_cb }, { NULL, "saturation",
				"set saturation (1-256)", INT, ov7670_saturation_cb }, { NULL,
				"hue", "set hue (-180 to 180)", INT, ov7670_hue_cb }, { NULL,
				"brightness", "set brightness (-4 to 4)", INT,
				ov7670_brightness_cb }, { NULL, "contrast",
				"set contrast (-4 to 4)", INT, ov7670_contrast_cb }, { NULL,
				"hflip", "flip horizontal (0=off/1=on)", INT, ov7670_hflip_cb },
		{ NULL, "vflip", "flip vertical (0=off/1=on)", INT, ov7670_vflip_cb }, {
				NULL, "light", "ov7670 light mode (0 - 5)", INT,
				ov7670_lightmode_cb }, { NULL, "night",
				"ov7670 night mode (0 - 3)", INT, ov7670_nightmode_cb }, { NULL,
				"effect", "special effects (0 - 8)", INT,
				ov7670_special_effects_cb },
		{ NULL, "gamma", "ov7670 gamma mode (0=disabled,1=slope1)", INT,
				ov7670_gamma_cb },
				{ NULL, "whitebalance",
				"ov7670 whitebalance (0,1,2)", INT, ov7670_whitebalance_cb },
				{NULL, "video", "video mode (0=off,1=on)", INT, videomode_cb },
				{NULL, "overlay", "display overlay (0=off,1=on)", INT, overlay_cb },
				{NULL, NULL, NULL, INT, NULL }
};

static int handle_command(uint8_t *cmdLine, size_t len) {
	int ret = sarg_init(&root, my_opts, "ESPILICAM");
	assert(ret == SARG_ERR_SUCCESS);
	// lots of redundant code here! //strcpy or memcpy would suffice
	size_t cmd_len = len;
	if (len > CMD_BUFFER_LEN)
		cmd_len = CMD_BUFFER_LEN;
	for (int i = 0; i < cmd_len; i++)
		telnet_cmd_buffer[i] = *(cmdLine + i);
	telnet_cmd_buffer[cmd_len - 1] = '\0';
	ESP_LOGD(TAG, "Processing telnet_cmd_buffer len=%d - contents=%s", cmd_len,
			(char* )telnet_cmd_buffer);
	if (telnet_cmd_buffer != NULL) {
		// next command will call sarg_parse and call callbacks as needed...
		ret = sarg_parse_command_buffer(&root, telnet_cmd_buffer, cmd_len);
		if (ret != SARG_ERR_SUCCESS) {
			ESP_LOGE(TAG, "Command parsing failed");
			sarg_destroy(&root);
			return -1;
		}
		// command has been parsed and executed!
		ESP_LOGD(TAG, "Command parser completed...");
	}
	sarg_destroy(&root);
	return 0;
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
	switch (event->event_id) {
	case SYSTEM_EVENT_STA_START:
		esp_wifi_connect();
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		s_ip_addr = event->event_info.got_ip.ip_info.ip;
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		/* This is a workaround as ESP32 WiFi libs don't currently
		 auto-reassociate. */
		esp_wifi_connect();
		xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
		break;
	default:
		break;
	}
	return ESP_OK;
}

static void initialise_wifi(void) {
	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT()
	;
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = { .sta = { .ssid = CONFIG_WIFI_SSID, .password =
			CONFIG_WIFI_PASSWORD, }, };
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_LOGI(TAG, "Connecting to \"%s\"", wifi_config.sta.ssid);
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true,
			portMAX_DELAY);
	ESP_LOGI(TAG, "Connected");
}

// COMMAND PARSER
uint8_t getHexVal(char c) {
	if (c >= '0' && c <= '9')
		return (uint8_t) (c - '0');
	else
		return (uint8_t) (c - 'A' + 10);
}

// just handle one client at a time... if a new client connects, reset these values
static bool telnet_client_id = 0;
static bool telnet_client_welcomed = false;
static bool telnet_client_connected = false;

static void handleTelnetConnectEvent(bool connectOrDisconnect,
		telnetUserData_t *telnetClient) {
	int clientId = telnetClient->sockfd;
	if (telnet_client_id != clientId) {
		// register new telnet client
		telnet_client_id = clientId;
		telnet_client_welcomed = false;
		telnet_client_connected = true;
	}
	if (!telnet_client_welcomed) {

		handle_command((uint8_t*) "help", 5); // extra /0 char!
		telnet_client_welcomed = true;
	}
}

static void recvData(uint8_t *buffer, size_t size,
		telnetUserData_t *telnetClient) {
	ESP_LOGD(TAG, "We received: %.*s from client id %d", size, buffer,
			telnetClient->sockfd);

	handle_command(buffer, size);
}

static void telnetTask(void *data) {
	ESP_LOGD(TAG, "Listening for telnet clients...");
	telnet_esp32_listenForClients(recvData, handleTelnetConnectEvent);
	ESP_LOGD(TAG, "stopping telnetTask");
	ESP_LOGI(TAG, "task stack: %d", uxTaskGetStackHighWaterMark(NULL));
	vTaskDelete(NULL);
}

// 8-bit logic - ref.
/*
 static void convert_yuv_line_to_565(uint8_t *srcline, uint8_t *destline, int byte_len) {

 uint16_t pixel565 = 0;
 uint16_t pixel565_2 = 0;
 for (int current_byte_pos = 0; current_byte_pos < byte_len; current_byte_pos += 4)
 {
 uint8_t y1, y2, u, v;
 y1 = srcline[current_byte_pos+3];
 v = srcline[current_byte_pos+2];
 y2 = srcline[current_byte_pos+1];
 u = srcline[current_byte_pos];
 pixel565 = fast_yuv_to_rgb565(y1,u,v);
 //pixel565 = __bswap_16(pixel565);
 pixel565_2 = fast_yuv_to_rgb565(y2,u,v);
 //pixel565 = __bswap_16(pixel565_2);
 destline[0] = pixel565 & 0xFF00 >> 8;
 destline[1] = pixel565 & 0xFF;
 //memcpy(destline, &pixel565, sizeof(uint16_t));
 destline +=2;
 destline[0] = pixel565_2 & 0xFF00 >> 8;
 destline[1] = pixel565_2 & 0xFF;
 //memcpy(destline, &pixel565_2, sizeof(uint16_t));
 destline +=2;
 }
 }
 */

// flip image
void convert_fb32bit_line_to_bmp565(uint32_t *srcline, uint8_t *destline,
		const int width, const camera_pixelformat_t format) {
	uint16_t pixel565 = 0;
	uint16_t pixel565_2 = 0;
	uint32_t long2px = 0;
	uint16_t *dptr;
	int current_src_pos = 0;
	int current_dest_pos = width * 2;

	for (int current_pixel_pos = 0; current_pixel_pos < width;
			current_pixel_pos += 2) {
		current_src_pos = current_pixel_pos / 2;
		long2px = srcline[current_src_pos];

		if (format == CAMERA_PF_YUV422) {
			uint8_t y1, y2, u, v;
			y1 = unpack(0, long2px);
			u = unpack(1, long2px);
			;
			y2 = unpack(2, long2px);
			v = unpack(3, long2px);

			pixel565 = fast_yuv_to_rgb565(y1, u, v);
			pixel565_2 = fast_yuv_to_rgb565(y2, u, v);

		} else if (format == CAMERA_PF_RGB565) {
			pixel565 = (unpack(0, long2px) << 8) | unpack(1, long2px);
			pixel565_2 = (unpack(2, long2px) << 8) | unpack(3, long2px);
		}

		current_dest_pos -= 2;
		dptr = (uint16_t *) &destline[current_dest_pos];
		*dptr = pixel565;
		current_dest_pos -= 2;
		dptr = (uint16_t *) &destline[current_dest_pos];
		*dptr = pixel565_2;
	}
}

// TODO: handle http request while videomode on

static void http_server_netconn_serve(struct netconn *conn) {
	struct netbuf *inbuf;
	char *buf;
	u16_t buflen;
	err_t err;
	/* Read the data from the port, blocking if nothing yet there.
	 We assume the request (the part we care about) is in one netbuf */
	err = netconn_recv(conn, &inbuf);
	if (err == ERR_OK) {
		netbuf_data(inbuf, (void**) &buf, &buflen);

		/* Is this an HTTP GET command? (only check the first 5 chars, since
		 there are other formats for GET, and we're keeping it very simple )*/
		if (buflen >= 5 && buf[0] == 'G' && buf[1] == 'E' && buf[2] == 'T'
				&& buf[3] == ' ' && buf[4] == '/') {

			// disable videomode (autocapture) to allow streaming...
			bool s_moviemode = is_moviemode_on();
			set_moviemode(false);

			/* Send the HTTP header
			 * subtract 1 from the size, since we dont send the \0 in the string
			 * NETCONN_NOCOPY: our data is const static, so no need to copy it
			 */
			netconn_write(conn, http_hdr, sizeof(http_hdr) - 1, NETCONN_NOCOPY);

			//check if a stream is requested.
			if (buf[5] == 's') {
				//Send mjpeg stream header
				err = netconn_write(conn, http_stream_hdr,
						sizeof(http_stream_hdr) - 1, NETCONN_NOCOPY);
				ESP_LOGD(TAG, "Stream started.");
				set_streaming(true);
				//Run while everyhting is ok and connection open.
				while (err == ERR_OK) {
					ESP_LOGD(TAG, "Capture frame");

					capture_request();
					capture_wait_finish();

					//spi_lcd_send();
					//spi_lcd_wait_finish();

					if (err != ESP_OK) {
						ESP_LOGD(TAG, "Camera capture failed with error = %d",
								err);
					} else {
						ESP_LOGD(TAG, "Done");
						// stream an image..
						if ((s_pixel_format == CAMERA_PF_RGB565)
								|| (s_pixel_format == CAMERA_PF_YUV422)) {
							// write mime boundary start
							err = netconn_write(conn, http_bitmap_hdr,
									sizeof(http_bitmap_hdr) - 1,
									NETCONN_NOCOPY);
							// write bitmap header
							int width = camera_get_fb_width();
							int height = camera_get_fb_height();
							int bytes_per_pixel = 2;

							char *bmp = bmp_create_header565(width, height);
							err = netconn_write(conn, bmp, sizeof(bitmap565),
									NETCONN_NOCOPY);
							free(bmp);
							// convert framebuffer on the fly...
							// only rgb and yuv...
							uint8_t *s_line = (uint8_t *) malloc(
									width * bytes_per_pixel);
							uint32_t *fbl; // address of current line/row in framebuffer
							fb_context_t fbc_http_server;

							for (int y = 0; y < height; y++) {
								int current_pixel_pos = y * width;
								fbl = (uint32_t*) framebuffer_pos_32(
										&fbc_http_server, current_pixel_pos);

								convert_fb32bit_line_to_bmp565(fbl, s_line,
										width, s_pixel_format);
								err = netconn_write(conn, s_line,
										width * bytes_per_pixel, NETCONN_COPY);
							}
							free(s_line);
						} else   // stream jpeg
						{
							err = netconn_write(conn, http_jpg_hdr,
									sizeof(http_jpg_hdr) - 1, NETCONN_NOCOPY);
							if (err == ERR_OK) {
								int size = camera_get_data_size();
								int bytes_per_pixel = 2;
								int bytes_in_line = camera_get_fb_width()
										* bytes_per_pixel;
								int current_pixel_pos = 0;

								uint32_t *fbl; // address of current line/row in framebuffer
								fb_context_t fbc_http_server;

								while (size > 0 && err == ERR_OK) {
									fbl = (uint32_t*) framebuffer_pos_32(
											&fbc_http_server,
											current_pixel_pos);

									err = netconn_write(conn, fbl,
											size % bytes_in_line, NETCONN_COPY);

									current_pixel_pos += bytes_in_line;
									size -= bytes_in_line;
								}
							}
						}
						if (err == ERR_OK) {
							//Send boundary to next jpeg
							err = netconn_write(conn, http_stream_boundary,
									sizeof(http_stream_boundary) - 1,
									NETCONN_NOCOPY);
						}
						vTaskDelay(30 / portTICK_RATE_MS);
					}
				}
				// notify system streaming completed...
				set_streaming(false);
				ESP_LOGD(TAG, "Stream ended.");
				ESP_LOGI(TAG, "task stack: %d",
						uxTaskGetStackHighWaterMark( NULL ));
			} else {
				if (s_pixel_format == CAMERA_PF_JPEG) {
					netconn_write(conn, http_jpg_hdr, sizeof(http_jpg_hdr) - 1,
							NETCONN_NOCOPY);
				} else if (s_pixel_format == CAMERA_PF_GRAYSCALE) {
					netconn_write(conn, http_pgm_hdr, sizeof(http_pgm_hdr) - 1,
							NETCONN_NOCOPY);
					if (memcmp(&buf[5], "pgm", 3) == 0) {
						char pgm_header[32];
						snprintf(pgm_header, sizeof(pgm_header),
								"P5 %d %d %d\n", camera_get_fb_width(),
								camera_get_fb_height(), 255);
						netconn_write(conn, pgm_header, strlen(pgm_header),
								NETCONN_COPY);
					} else {
						char outstr[120];
						get_image_mime_info_str(outstr);
						netconn_write(conn, outstr, sizeof(outstr) - 1,
								NETCONN_NOCOPY);
						//netconn_write(conn, http_yuv422_hdr, sizeof(http_yuv422_hdr) - 1, NETCONN_NOCOPY);
					}
				} else if (s_pixel_format == CAMERA_PF_RGB565) {
					netconn_write(conn, http_bitmap_hdr,
							sizeof(http_bitmap_hdr) - 1, NETCONN_NOCOPY);
					if (memcmp(&buf[5], "bmp", 3) == 0) {
						char *bmp = bmp_create_header565(camera_get_fb_width(),
								camera_get_fb_height());
						err = netconn_write(conn, bmp, sizeof(bitmap565),
								NETCONN_COPY);
						free(bmp);
					} else {
						char outstr[120];
						get_image_mime_info_str(outstr);
						netconn_write(conn, outstr, sizeof(outstr) - 1,
								NETCONN_NOCOPY);
						//netconn_write(conn, http_yuv422_hdr, sizeof(http_yuv422_hdr) - 1, NETCONN_NOCOPY);
					}
				} else if (s_pixel_format == CAMERA_PF_YUV422) {
					if (memcmp(&buf[5], "bmp", 3) == 0) {
						//PAUSE_DISPLAY = true;
						// send YUV converted to 565 2bpp for now...
						netconn_write(conn, http_bitmap_hdr,
								sizeof(http_bitmap_hdr) - 1, NETCONN_NOCOPY);
						char *bmp = bmp_create_header565(camera_get_fb_width(),
								camera_get_fb_height());
						err = netconn_write(conn, bmp, sizeof(bitmap565),
								NETCONN_COPY);
						free(bmp);
					} else {
						char outstr[120];
						get_image_mime_info_str(outstr);
						netconn_write(conn, outstr, sizeof(outstr) - 1,
								NETCONN_NOCOPY);
						//netconn_write(conn, http_yuv422_hdr, sizeof(http_yuv422_hdr) - 1, NETCONN_NOCOPY);
					}
				} else {
					char outstr[120];
					get_image_mime_info_str(outstr);
					netconn_write(conn, outstr, sizeof(outstr) - 1,
							NETCONN_NOCOPY);
				}
				// handle non streaming images (http../get and http:../bmp )

				ESP_LOGD(TAG, "Image requested.");
				//ESP_LOGI(TAG, "task stack: %d", uxTaskGetStackHighWaterMark(NULL));

				// debug display will just send the current framebuffer, including overlay
				if (!debugDisplay) {
					bool s_moviemode = is_moviemode_on();
					set_moviemode( false);
					capture_request();
					capture_wait_finish();
					set_moviemode(s_moviemode);
				}
				//ESP_LOGI(TAG, "task stack: %d", uxTaskGetStackHighWaterMark(NULL));

				if (err != ESP_OK) {
					ESP_LOGD(TAG, "Camera capture failed with error = %d", err);
				} else {
					ESP_LOGD(TAG, "Done");
					//ESP_LOGI(TAG, "task stack: %d", uxTaskGetStackHighWaterMark(NULL));
					//Send jpeg
					if ((s_pixel_format == CAMERA_PF_RGB565)
							|| (s_pixel_format == CAMERA_PF_YUV422)) {
						ESP_LOGD(TAG,
								"Converting framebuffer to RGB565 requested, sending...");

						int width = camera_get_fb_width();
						int height = camera_get_fb_height();
						int bytes_per_pixel = 2;

						uint8_t *s_line = (uint8_t *) malloc(
								width * bytes_per_pixel);
						uint32_t *fbl; // address of current line/row in framebuffer
						fb_context_t fbc_http_server;

						for (int y = 0; y < height; y++) {
							int current_pixel_pos = y * width;
							fbl = (uint32_t*) framebuffer_pos_32(
									&fbc_http_server, current_pixel_pos);

							convert_fb32bit_line_to_bmp565(fbl, s_line, width,
									s_pixel_format);
							err = netconn_write(conn, s_line,
									width * bytes_per_pixel, NETCONN_COPY);
							if (err != ERR_OK) {
								ESP_LOGD(TAG, "Error on send line %d = 0x%02x",
										y, err);
								break;
							}
						}
						free(s_line);
					} else {
						// send jpeg
						int size = camera_get_data_size();
						int bytes_per_pixel = 2;
						int bytes_in_line = camera_get_fb_width()
								* bytes_per_pixel;
						int current_pixel_pos = 0;

						uint32_t *fbl; // address of current line/row in framebuffer
						fb_context_t fbc_http_server;

						while (size > 0 && err == ERR_OK) {
							fbl = (uint32_t*) framebuffer_pos_32(
									&fbc_http_server, current_pixel_pos);

							err = netconn_write(conn, fbl, size % bytes_in_line,
									NETCONN_COPY);

							current_pixel_pos += bytes_in_line;
							size -= bytes_in_line;

							if (err != ERR_OK) {
								ESP_LOGD(TAG,
										"Error on send line, remains %d bytes = 0x%02x",
										size, err);
								break;
							}
						}
					}
				} // handle .bmp and std gets...

			}
			// end GET request:
			set_moviemode(s_moviemode);
		}
	}
	/* Close the connection (server closes in HTTP) */
	netconn_close(conn);
	/* Delete the buffer (netconn_recv gives us ownership,
	 so we have to make sure to deallocate the buffer) */
	netbuf_delete(inbuf);
}

static void http_server(void *pvParameters) {
	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, NULL, 80);
	netconn_listen(conn);
	do {
		err = netconn_accept(conn, &newconn);
		if (err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
	} while (err == ERR_OK);
	netconn_close(conn);
	netconn_delete(conn);
}

static spi_bus_config_t buscfg = { .miso_io_num = PIN_NUM_MISO, .mosi_io_num =
		PIN_NUM_MOSI, .sclk_io_num = PIN_NUM_CLK, .quadwp_io_num = -1,
		.quadhd_io_num = -1 };

static spi_device_interface_config_t devcfg = {
//    .clock_speed_hz=10000000,               //Clock out at 10 MHz - too slow
		.clock_speed_hz = 20000000, // works well... but can it hold out for 10+ mins?
//    .clock_speed_hz=26000000,               //Clock out at 26 MHz. Yes, that's heavily overclocked.
		.mode = 0,                                //SPI mode 0
		.spics_io_num = PIN_NUM_CS,               //CS pin
		.queue_size = 7,  //We want to be able to queue 7 transactions at a time
		.pre_cb = ili_spi_pre_transfer_callback, //Specify pre-transfer callback to handle D/C line
		};

#define NVS_SETUP_STARTUP_DELAY 500
#define WIFI_TASK_STARTUP_DELAY 1000
#define CAM_CFG_TASK_STARTUP_DELAY 200
#define CAPTURE_TASK_STARTUP_DELAY 200
#define FRAMEBUFFER_TASK_STARTUP_DELAY 100
#define FLASHSCREEN_TASK_STARTUP_DELAY 1000
#define TELNET_TASK_STARTUP_DELAY 100
#define HTTP_TASK_STARTUP_DELAY 100

#define MOVIEMODE_STARTUP_WAIT_TIME 4000
#define STARTUP_DISPLAY_OVERLAY true
#define STARTUP_MOVIEMODE true

void app_main() {
	size_t free8start, free32start, free8, free32;

	esp_log_level_set("wifi", ESP_LOG_WARN);
	esp_log_level_set("gpio", ESP_LOG_WARN);

	ESP_LOGI(TAG, "Starting nvs_flash_init");
	nvs_flash_init();

	vTaskDelay(NVS_SETUP_STARTUP_DELAY / portTICK_RATE_MS);

	ESP_LOGI(TAG, "Starting ESPILICAM");
	ESP_LOGI(TAG, "Free heap: %u", xPortGetFreeHeapSize());

	initialise_wifi();

	// VERY UNSTABLE without this delay after init'ing wifi...
	// however, much more stable with a new Power Supply
	vTaskDelay(WIFI_TASK_STARTUP_DELAY / portTICK_RATE_MS);

	ESP_LOGI(TAG, "Wifi Initialized...");
	ESP_LOGI(TAG, "Free heap: %u", xPortGetFreeHeapSize());

	esp_err_t ret;

	//Initialize the SPI bus
	//ESP_LOGI(TAG, "Call spi_bus_initialize");
	ret = spi_bus_initialize(HSPI_HOST, &buscfg, 1);
	assert(ret==ESP_OK);
	//Attach the LCD to the SPI bus
	//ESP_LOGI(TAG, "Call spi_bus_add_device");
	ret = spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
	assert(ret==ESP_OK);
	//Initialize the LCD
	//ESP_LOGI(TAG, "Call ili_init");
	ili_init(spi);

	// camera init

	esp_err_t err = camera_probe(&config, &camera_model);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Camera probe failed with error 0x%x", err);
		return;
	}
	if (camera_model == CAMERA_OV7725) {
		ESP_LOGI(TAG, "Detected OV7725 camera, using grayscale bitmap format");
		s_pixel_format = CAMERA_PIXEL_FORMAT;
		config.frame_size = CAMERA_FRAME_SIZE;
	} else if (camera_model == CAMERA_OV7670) {
		ESP_LOGI(TAG, "Detected OV7670 camera");
		s_pixel_format = CAMERA_PIXEL_FORMAT;
		config.frame_size = CAMERA_FRAME_SIZE;
	} else if (camera_model == CAMERA_OV2640) {
		ESP_LOGI(TAG, "Detected OV2640 camera, using JPEG format");
		s_pixel_format = CAMERA_PF_JPEG;
		config.frame_size = CAMERA_FS_VGA;
		config.jpeg_quality = 15;
	} else {
		ESP_LOGE(TAG, "Camera not supported");
		return;
	}

	free32 = heap_caps_get_largest_free_block( MALLOC_CAP_32BIT);
	free8 = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT);
	free8start = heap_caps_get_minimum_free_size( MALLOC_CAP_8BIT);
	free32start = heap_caps_get_minimum_free_size( MALLOC_CAP_32BIT);

	ESP_LOGI(TAG, "Free heap: %u", xPortGetFreeHeapSize());
	ESP_LOGI(TAG,
			"Free (largest free blocks) 8bit-capable memory : %dK, 32-bit capable memory %dK\n",
			free8, free32);
	ESP_LOGI(TAG,
			"Free (min free size) 8bit-capable memory : %dK, 32-bit capable memory %dK\n",
			free8start, free32start);

	config.pixel_format = s_pixel_format;
	err = camera_init(&config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
		return;
	}

	vTaskDelay(CAM_CFG_TASK_STARTUP_DELAY / portTICK_RATE_MS);

	dispSem = xSemaphoreCreateBinary();
	dispDoneSem = xSemaphoreCreateBinary();
	espilicam_event_group = xEventGroupCreate();

	xSemaphoreGive(dispDoneSem);
	ESP_LOGD(TAG, "Starting ILI9341 display task...");
	xTaskCreatePinnedToCore(&push_framebuffer_to_tft, "push_framebuffer_to_tft",
			6144, NULL, 5, NULL, 1);

	// TODO: INITIAL OVERLAY / FLASH SCREEN
	// CHECK IF SEPERATE TASK IS NEEDED OR
	// INTEGRATE INTO FB / CAPTURE / TASKS..

	// xSemaphoreGive(dispSem);
	setupInitialGraphics();
	xSemaphoreGive(dispSem);
	// TODO: handle messaging to graphics layer
	// copy ip address to graphics
	gfx_s_ip_addr = s_ip_addr;

	displayHelpScreen();
	xSemaphoreGive(dispSem);
	ESP_LOGD(TAG, "Help screen init...");
	vTaskDelay(FLASHSCREEN_TASK_STARTUP_DELAY / portTICK_RATE_MS);

	captureSem = xSemaphoreCreateBinary();
	captureDoneSem = xSemaphoreCreateBinary();

	ESP_LOGD(TAG, "Starting OV7670 capture task...");
	xTaskCreatePinnedToCore(&captureTask, "captureTask", 2048, NULL, 5, NULL,
			1);

	vTaskDelay(CAPTURE_TASK_STARTUP_DELAY / portTICK_RATE_MS);

	ESP_LOGD(TAG, "Starting http_server task...");
	// keep an eye on stack... 5784 min with 8048 stck size last count..
	xTaskCreatePinnedToCore(&http_server, "http_server", 6144, NULL, 5, NULL,
			1);

	ESP_LOGI(TAG, "open http://" IPSTR "/bmp for single image/bitmap image",
			IP2STR(&s_ip_addr));
	ESP_LOGI(TAG,
			"open http://" IPSTR "/stream for multipart/x-mixed-replace stream of bitmaps",
			IP2STR(&s_ip_addr));
	ESP_LOGI(TAG,
			"open http://" IPSTR "/get for raw image as stored in framebuffer ",
			IP2STR(&s_ip_addr));

	ESP_LOGD(TAG, "Starting telnetd task...");
	// keep an eye on this - stack free was at 4620 at min with 8048
	xTaskCreatePinnedToCore(&telnetTask, "telnetTask", 6144, NULL, 5, NULL, 1);

	ESP_LOGI(TAG,
			"telnet to \"telnet " IPSTR "\" to access command console, type \"help\" for commands",
			IP2STR(&s_ip_addr));

	free32 = heap_caps_get_largest_free_block( MALLOC_CAP_32BIT);
	free8 = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT);
	free8start = heap_caps_get_minimum_free_size( MALLOC_CAP_8BIT);
	free32start = heap_caps_get_minimum_free_size( MALLOC_CAP_32BIT);

	ESP_LOGI(TAG, "Free heap: %u", xPortGetFreeHeapSize());
	ESP_LOGI(TAG,
			"Free (largest free blocks) 8bit-capable memory : %dK, 32-bit capable memory %dK\n",
			free8, free32);
	ESP_LOGI(TAG,
			"Free (min free size) 8bit-capable memory : %dK, 32-bit capable memory %dK\n",
			free8start, free32start);

	ESP_LOGI(TAG, "task stack: %d", uxTaskGetStackHighWaterMark(NULL));
	ESP_LOGI(TAG, "Camera demo ready.");

	// startup the machinery
	if (STARTUP_DISPLAY_OVERLAY) set_overlay(true);
	vTaskDelay(MOVIEMODE_STARTUP_WAIT_TIME / portTICK_RATE_MS);
	if (STARTUP_MOVIEMODE) set_moviemode(true);
	capture_request();


}
