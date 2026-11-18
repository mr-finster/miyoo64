/* Exercise the actual first-start ROM checks; the test ROMs remain private. */
#include "../src/port/hh/hh_convert.c"
#include <stdarg.h>

void HhRomScreen_Progress(const char* a, const char* b, const char* c, const char* d, int e, const char* f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
void port_log_always(const char* fmt, ...) { (void)fmt; }

int main(int argc, char** argv) {
    char why[96] = {0};
    int order = -1, quick, verified = 0;
    Rom rom;
    if (argc != 3) return 2;
    quick = quick_check(argv[1], why, sizeof(why), &order);
    if (quick == 1) {
        rom.f = fopen(argv[1], "rb");
        if (!rom.f) return 2;
        rom.order = order;
        verified = rom_verify(&rom);
        fclose(rom.f);
    }
    printf("quick=%d verified=%d order=%d\n", quick, verified, order);
    return verified == atoi(argv[2]) ? 0 : 1;
}
