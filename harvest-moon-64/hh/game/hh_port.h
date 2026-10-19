/* What the game side (decomp headers) and the host side (system headers) of the port share. Plain C types only. */
#ifndef HH_PORT_H
#define HH_PORT_H

/* host side (hh/src) */
void hh_gfx_init(void);
void hh_gfx_task(void *gfx_list, int swap);
void hh_display_on(int on);
void hh_host_retrace(int main_wait); /* main_wait: mainLoop's wait between steps */
void hh_input_read(unsigned short *button, signed char *stick_x, signed char *stick_y);
void hh_rom_read(unsigned int rom_addr, void *dst, unsigned int size);
void hh_sram_read(unsigned int dev_addr, void *dst, unsigned int size);
void hh_sram_write(unsigned int dev_addr, const void *src, unsigned int size);
void hh_log_game(const char *fmt, ...);

/* game side (hh/game) */
void hh_wait_retrace(void);
void hh_game_main(void);

#endif
