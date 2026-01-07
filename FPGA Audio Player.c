#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <system.h>
#include <sys/alt_alarm.h>
#include <sys/alt_irq.h>
#include <io.h>
#include <stdbool.h>

#include "fatfs.h"
#include "diskio.h"

#include "ff.h"
#include "monitor.h"
#include "uart.h"

#include "alt_types.h"

#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>
#include "altera_avalon_timer.h"
#include "altera_avalon_timer_regs.h"

//DEFINE VARIABLES
#define MAX_SONGS 20

DIR dir;   // Directory object
FIL file;  // File object
FIL File1, File2;               /* File objects */

alt_up_audio_dev *audio_dev;
unsigned int l_buf;
unsigned int r_buf;
bool playing = false;
unsigned int position = 0;

static void timerInterruptFunction(void *context, alt_32 id);
static void btn_interrupt(void *context, alt_32 id);
int button_pressed = 0;
int count_released = 0;
int button = 0;

char filenames[MAX_SONGS][20];  // Array to store wave names
unsigned long fileSizes[MAX_SONGS];
int songCount = 0;  // Counter for the number of .wav files found
int currentTrack = 0;
char music_status[50];
char speed_status[50];

static alt_alarm alarm;
static unsigned long Systick = 0;
static volatile unsigned short Timer;   /* 1000Hz increment timer */
uint8_t Buff[8192] __attribute__ ((aligned(4)));  /* Working buffer */
FATFS Fatfs[_VOLUMES];          /* File system object for each logical drive */

//Phase 2 Variables
int fifospace;
char *ptr, *ptr2;
long p1, p2, p3;
uint8_t res, b1, drv = 0;
uint16_t w1;
uint32_t s1, s2, cnt, blen = sizeof(Buff);
static const uint8_t ft[] = { 0, 12, 16, 32 };
uint32_t ofs = 0, sect = 0, blk[2];
FATFS *fs;

//HELPER FUNCTIONS
static alt_u32 TimerFunction (void *context)
{
	static unsigned short wTimer10ms = 0;

	(void)context;

	Systick++;
	wTimer10ms++;
	Timer++; /* Performance counter for this module */

	if (wTimer10ms == 10)
	{
		wTimer10ms = 0;
		ffs_DiskIOTimerproc();  /* Drive timer procedure of low level disk I/O module */
	}

	return(1);
}

static void IoInit(void)
{
	uart0_init(115200);
	ffs_DiskIOInit();
	alt_alarm_start(&alarm, 1, &TimerFunction, NULL);
}

static void put_rc(FRESULT rc)
{
	const char *str =
			"OK\0" "DISK_ERR\0" "INT_ERR\0" "NOT_READY\0" "NO_FILE\0" "NO_PATH\0"
			"INVALID_NAME\0" "DENIED\0" "EXIST\0" "INVALID_OBJECT\0" "WRITE_PROTECTED\0"
			"INVALID_DRIVE\0" "NOT_ENABLED\0" "NO_FILE_SYSTEM\0" "MKFS_ABORTED\0" "TIMEOUT\0"
			"LOCKED\0" "NOT_ENOUGH_CORE\0" "TOO_MANY_OPEN_FILES\0";
	FRESULT i;

	for (i = 0; i != rc && *str; i++) {
		while (*str++);
	}
	xprintf("rc=%u FR_%s\n", (uint32_t) rc, str);
}

//verify whether or not its a wave file
int isWav(char *filename) {
	int len = strlen(filename);
	printf("Checking file: %s\n", filename);

	if (len >= 4 && strcasecmp(&filename[len - 4], ".wav") == 0) {
		printf("It is a .wav file.\n");
		return 1;
	}
	printf("It is NOT a .wav file.\n");
	return 0;
}

//phase 3 list of files
FRESULT listFilesOnSDCard() {
	FRESULT directory;
	FILINFO fno;
	char path[] = "/";  // Path to the root directory on SD card

	printf("Listing files on SD card...\n");
	directory = f_opendir(&dir, path);  // open directory
	if (directory != FR_OK) {
		printf("Error opening directory (Error Code: %d)\n", directory);
		return directory;  // cant open directory
	}

	printf("Scanning SD card for .wav files...\n");
	while ((directory = f_readdir(&dir, &fno)) == FR_OK && fno.fname[0]) {
		printf("Found file: %s\n", fno.fname); //files that are found
		if (isWav(fno.fname)) {
			strncpy(filenames[songCount], fno.fname, sizeof(filenames[songCount]) - 1);
			fileSizes[songCount] = fno.fsize;
			songCount++;
			printf("Adding .wav file: %s (Size: %lu bytes)\n", filenames[songCount - 1], fileSizes[songCount - 1]);
			if (songCount >= MAX_SONGS) {
				break;
			}
		}
	}

	printf("Found %d .wav files.\n", songCount);
	return directory; //return the directory
}

static void timerInterruptFunction(void* context, alt_32 id){
	int current_value = IORD(BUTTON_PIO_BASE, 0);
	IOWR(LED_PIO_BASE, 0, 0x1);
	if (button == 0 && current_value != 0xF){
		button_pressed = current_value;
		button = 1;
	}
	if(current_value == 0xF){
		count_released++;
		if (count_released < 15)
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
	}
	else{
		count_released = 0;
		IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
	}
	if (count_released == 15){
			IOWR(LED_PIO_BASE, 0, 0x0);
			count_released = 0;
			button = 0;
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);
			IOWR(BUTTON_PIO_BASE, 2, 0xF);
	}
}

static void btn_interrupt(void *context, alt_32 id){
	IOWR(BUTTON_PIO_BASE, 3, 0);
	IOWR(BUTTON_PIO_BASE, 2, 0);
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
}

static void display_file(){
	FILE *lcd = fopen("/dev/lcd_display", "w");
	if (lcd == NULL) {
		printf("Error: Failed to open the LCD device\n");
	}
	fprintf(lcd, "\x1b[2J"); //clears the lcd screen
	char displayString[40];
	snprintf(displayString, sizeof(displayString), "Track %d: %s", currentTrack + 1 ,filenames[currentTrack]);
	fprintf(lcd, "%s", displayString);  // Write the wave name to the LCD

	fclose(lcd);  // Close the LCD device
	printf("Displaying song on LCD: %s\n", filenames[currentTrack]);
}
static void display_music(){
	FILE *lcd2 = fopen("/dev/lcd_display", "w");
	if (lcd2 == NULL) {
		printf("Error: Failed to open the LCD device\n");
	}
	fprintf(lcd2, "\x1b[2;1H");  //cursor to second line
	fprintf(lcd2, "                    ");//clear

	fprintf(lcd2, "\x1b[2;1H");
	fprintf(lcd2, "%s", music_status);
	fclose(lcd2);  // Close the LCD device
	printf("Music Status: %s\n", music_status);
}

static void display_speed(){
	FILE *lcd3 = fopen("/dev/lcd_display", "w");
	if (lcd3 == NULL) {
		printf("Error: Failed to open the LCD device\n");
	}
	fprintf(lcd3, "\x1b[2;1H");
	fprintf(lcd3, "                    ");//clear

	fprintf(lcd3, "\x1b[2;1H");
	fprintf(lcd3, "%s", speed_status);
	fclose(lcd3);  // Close the LCD device
	printf("Music Status: %s\n", speed_status);
}

void open_audio_file(int track) {
    FRESULT res;
    res = f_open(&File1, filenames[track], FA_READ); //open audio file
    if (res != FR_OK) {
        printf("Error opening file: %d\n", res);
        return;
    }
    printf("Opened file: %s\n", filenames[track]);

    //play the file
    play(&File1);
}
void stop() {
    position = 0;  //reset buffer position
    File1.fptr = 0;  // Reset the file pointer
    playing = false;
}

void next() {
    currentTrack++;
    if (currentTrack >= songCount) {
        currentTrack = 0;  //go back to first track at the end
    }
    display_file();
    open_audio_file(currentTrack);
}

void previous() {
    currentTrack--;
    if (currentTrack < 0) {
        currentTrack = songCount - 1;  //come back to the last track
    }
    display_file();
    open_audio_file(currentTrack);
}
void play(FIL* file) {
    unsigned int bytes_read;
    int chunk;
    uint8_t buf[2420192];

    //read speed based on switch state
    int sw0 = IORD(SWITCH_PIO_BASE, 0) & 0x1;
    int sw1 = IORD(SWITCH_PIO_BASE, 0) & 0x2;

    int speed = 4;   // normal
    int mono = 0;

    if (!sw0 && !sw1) {//mono mode
        alt_printf("Mono\n");
        speed = 4;
        mono = 1;
        memset(speed_status, 0, sizeof(music_status));
        strcpy(speed_status, "PBACK–MONO");
        display_speed();
    } else if (sw0 && !sw1) {//double speed faster
        alt_printf("Stereo, Double Speed\n");
        speed = 8;
        mono = 0;
        memset(speed_status, 0, sizeof(music_status));
        strcpy(speed_status, "PBACK–DBL SPD");
        display_speed();
    } else if (!sw0 && sw1) {//half speed slower
        alt_printf("Stereo, Half Speed\n");
        speed = 2;
        mono = 0;
        memset(speed_status, 0, sizeof(music_status));
        strcpy(speed_status, "PBACK–HALF SPD");
        display_speed();
    } else {//normal
        alt_printf("Normal Speed\n");
        speed = 4;
        mono = 0;
        memset(speed_status, 0, sizeof(music_status));
        strcpy(speed_status, "PBACK-NORM SPD");
        display_speed();
    }

    // process the file in chunks if playing
    while (1) {  //continuously poll and process the chunks

        if (button_pressed == 13) { //play/pause buttons
            button_pressed = 0;

            if (!playing) {
                // continuing playing from same position
                File1.fptr = position; //saved position
                alt_printf("Resuming Playback from position %lu\n", File1.fptr);
                display_speed();
                playing = true;  //state to playing
            } else {
                //pause playback and save current position
                position = File1.fptr;
                alt_printf("Pausing Playback at position %lu\n", position);
                memset(music_status, 0, sizeof(music_status));
                strcpy(music_status, "PAUSED");
                display_music();
                playing = false;  //state to paused
            }
        }

        //check if playing, then continue reading and processing the audio data
        if (playing) {
            chunk = (File1.fsize - File1.fptr > sizeof(buf)) ? sizeof(buf) : File1.fsize - File1.fptr;

            // read a chunk of the file into the buffer
            FRESULT res = f_read(file, buf, chunk, &bytes_read);
            if (res != FR_OK || bytes_read == 0) {
                printf("Read error or EOF\n");
                break;
            }

            // process the read bytes in groups of 4 bytes (2 bytes for left and right channels)
            for (int i = 0; i + 3 < bytes_read; i += speed) {
                // check for Play/Pause button press during buffer processing
                if (button_pressed == 13) {  //Play/Pause Button
                    button_pressed = 0;
                    position = File1.fptr;  //current position
                    alt_printf("Pausing Playback at position %lu\n", position);
                    memset(music_status, 0, sizeof(music_status));
					strcpy(music_status, "PAUSED");
					display_music();
                    playing = false;  //set state to paused
                    break;  //exit the loop immediately after pausing
                }
				if (button_pressed == 11) {  //stop Button
					button_pressed = 0;
					stop();
					memset(music_status, 0, sizeof(music_status));
					strcpy(music_status, "STOPPED");//print to lcd second line
					display_music();
					printf("Stopped");
					break;
				}

				if (button_pressed == 14) {  //next track
					button_pressed = 0;
					next();
					strcpy(music_status, "STOPPED");//print to lcd second line
					display_music();

				}

				if (button_pressed == 7) {  // prev track
					button_pressed = 0;
					previous();
					strcpy(music_status, "STOPPED");//print to lcd second line
					display_music();
				}

                uint16_t left_sample = buf[i] | (buf[i + 1] << 8);   // Left channel
                uint16_t right_sample = buf[i + 2] | (buf[i + 3] << 8); // Right channel

                // wait for space in the left and right FIFO and write left and right sample
                while (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT) == 0);
                alt_up_audio_write_fifo(audio_dev, &left_sample, 1, ALT_UP_AUDIO_LEFT);
                while (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT) == 0);
                alt_up_audio_write_fifo(audio_dev, &right_sample, 1, ALT_UP_AUDIO_RIGHT);
            }
            if(!playing){
            	break;
            }
            File1.fptr += bytes_read;
        } else {
            continue;  //cont polling the buttons if paused
        }
    }
    alt_printf("Playback done\n");
    stop();
}


//MAIN FUNCTION
int main(void){
	// open the Audio port
	audio_dev = alt_up_audio_open_dev ("/dev/Audio");
	if ( audio_dev == NULL){
		alt_printf ("Error: could not open audio device \n");
	}
	else{
		alt_printf ("Opened audio device \n");
	}

	IoInit();

	// tentative code for di 0
	uint8_t drv = 0;  // specify the drive number (0 in this case)
	DSTATUS res = disk_initialize(drv);  // Initialize the disk

	// Check the result of the initialization
	if (res == RES_OK) {
		xprintf("Disk %d initialized successfully.\n", drv);
	} else {
		xprintf("Error initializing disk %d. Error code: %d\n", drv, res);
	}

	//tentative code for fi 0
	FRESULT res1;
	// Force initialize logical drive 0 by mounting it
	res1 = f_mount(drv, &Fatfs[drv]);  // Mount the drive (drive 0 in this case)
	// Output the result code of the operation
	put_rc(res1);
	//read files from sd card
	listFilesOnSDCard();

	//BUTTON HANDLING
	alt_irq_register(TIMER_0_IRQ, (void*)0, timerInterruptFunction);
	alt_irq_register(BUTTON_PIO_IRQ, (void*)0, btn_interrupt);
	IOWR(BUTTON_PIO_BASE, 3, 0);
	IOWR(BUTTON_PIO_BASE, 2, 0xF);
	IOWR_ALTERA_AVALON_TIMER_STATUS(TIMER_0_BASE, 0);
	IOWR_ALTERA_AVALON_TIMER_PERIODH(TIMER_0_BASE, 0x2);
	IOWR_ALTERA_AVALON_TIMER_PERIODL(TIMER_0_BASE, 0xFFFF);
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);
	int x = 0;
	display_file();
	strcpy(music_status, "STOPPED");//print to lcd second line
	display_music();
	while(1){
		if (button_pressed == 7){
			//backward
			printf("pb 3\n");
			currentTrack--;
			if (currentTrack < 0) {
				currentTrack = songCount - 1;
			}
			display_file();
			strcpy(music_status, "STOPPED");//print to lcd second line
			display_music();
			button_pressed = 0;
		}
		else if (button_pressed == 11){
			//stop
			printf("pb 2\n");
			button_pressed = 0;
			memset(music_status, 0, sizeof(music_status));
			strcpy(music_status, "STOPPED");
			display_music();
		}
		else if (button_pressed == 13){
			//play and pause
			printf("pb 1\n");
			open_audio_file(currentTrack);
			button_pressed = 0;
		}
		else if (button_pressed == 14){
			//forward
			printf("pb 0\n");
			currentTrack++;
			if (currentTrack >= songCount) {
				currentTrack = 0;
			}
			display_file();
			strcpy(music_status, "STOPPED");//print to lcd second line
			display_music();
			button_pressed = 0;
		}
	}

	return 0;

}
