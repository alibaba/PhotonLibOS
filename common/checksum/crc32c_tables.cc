// an offline generator for crc32c shift tables (L&R) of power-of-two sizes

#include <stdio.h>
#include <inttypes.h>

static uint32_t crctbl[256];
static uint32_t shift_table_power2[40];
static uint32_t ishift_table_power2[40];
#define POLY 0x82F63B78

// return c ? val : 0 ;
#define CVAL(c, val) (-!!(c) & val)

void GenTbl(void)                       /* generate crc table */
{
    uint32_t crc, c, i;
    for(c = 0; c < 0x100; c++){
        crc = c;
        for(i = 0; i < 8; i++)
            crc = (crc>>1) ^ CVAL(crc&1, POLY);
        crctbl[c] = crc;
    }
}

uint32_t GenCrc(uint8_t * bfr, size_t size) /* generate crc */
{
    uint32_t crc = 0u;
    while(size--)
        crc = (crc>>8) ^ crctbl[crc & 0xff ^ *bfr++];
    return(crc);
}

/* carryless multiply modulo crc */
uint32_t MpyModCrc(uint32_t a, uint32_t b) /* (a*b)%crc */
{
    uint32_t pd = 0, i;
    for(i = 0; i < 32; i++){
        pd = (pd>>1) ^ CVAL(pd&1, POLY) ^ CVAL(b&1, a);
        b >>= 1;
    }
    return pd;
}

uint32_t PowModCrc(uint64_t p);

void gen_shift_table_power2() {
    uint32_t x = 0x1u << 30;      // shift by 1
    shift_table_power2[0] = x;
    printf("0x%08x, ", x);
    for (uint32_t i = 1; i < 40; ++i) {
        x = shift_table_power2[i] = MpyModCrc(x, x);
        printf("0x%08x, ", x);
        if (i%4 == 3) printf("\n");
    }

    puts("inverse table");

    x = -1u;
    x = PowModCrc(x-1-1); // inverse shift by 1
    ishift_table_power2[0] = x;
    printf("0x%08x, ", x);
    for (uint32_t i = 1; i < 40; ++i) {
        x = ishift_table_power2[i] = MpyModCrc(x, x);
        printf("0x%08x, ", x);
        if (i%4 == 3) printf("\n");
    }

    for (uint32_t i = 0; i < 40; ++i) {
        printf("0x%08x, ", MpyModCrc(shift_table_power2[i],
                                    ishift_table_power2[i]));
        if (i%4 == 3) printf("\n");
    }
}

/* exponentiate by repeated squaring modulo crc */
uint32_t PowModCrc(uint64_t p)          /* pow(2,p)%crc */
{
    uint32_t* table = shift_table_power2;
    if (p >> 63) {  // p < 0
        p = -p;
        table = ishift_table_power2;
    }
    uint32_t prd = 0x1u << 31;                    /* current product */
    for(; p; p &= p-1) {
        uint32_t i = __builtin_ctzll(p) % 31;
        // printf("%d, ", i);
        prd = MpyModCrc(prd, table[i]);
    }
    // printf("\n");
    return prd;
}

void print_shift_talbes() {
    printf("0x%08x, 0x%08x\n", PowModCrc(-1ull), PowModCrc(-1u-1));
    puts("const static uint32_t crc32c_lshift_table_hw[32] = {");
    for (uint32_t i = 0; i < 32; ++i) {
        printf("0x%08x, ", PowModCrc((1ull << (i+3)) - 32 - 1));
        if (i % 4 == 3) puts("");
    }
    puts("};");

    puts("const static uint32_t crc32c_rshift_table_hw[32] = {");
    for (uint32_t i = 0; i < 32; ++i) {
        printf("0x%08x, ", PowModCrc(-(1ull << (i+3)) - 32 - 1));
        if (i % 4 == 3) puts("");
    }
    puts("};");

    puts("const static uint32_t crc32c_lshift_table_sw[32] = {");
    for (uint32_t i = 0; i < 32; ++i) {
        printf("0x%08x, ", PowModCrc((1ull << (i+3))));
        if (i % 4 == 3) puts("");
    }
    puts("};");

    puts("const static uint32_t crc32c_rshift_table_sw[32] = {");
    for (uint32_t i = 0; i < 32; ++i) {
        printf("0x%08x, ", PowModCrc(-(1ull << (i+3))));
        if (i % 4 == 3) puts("");
    }
    puts("};");
}

/*  message 14 data, 18 zeroes */
/*  parities = crc cycled backwards 18 bytes */

int main()
{
    uint32_t pmr, crc, par;
    uint8_t msg[32] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

    GenTbl();                           /* generate crc table */
    const uint32_t num_0 = 15;
    gen_shift_table_power2();
    print_shift_talbes();
    pmr = PowModCrc(-(num_0*8ull));         /* pmr = pow(2,-1-18*8)%crc */
    crc = GenCrc(msg, 14+num_0);              /* generate crc including 18 zeroes */
    par = MpyModCrc(crc, pmr);          /* par = (crc*pmr)%crc = new crc */
    crc = GenCrc(msg, 14);              /* generate crc for shortened msg */
    printf("%08x %08x\n", par, crc);    /* crc == par */
    *(uint32_t*)&msg[14] = par;
    crc = GenCrc(msg, 18);              /* crc == 0 */
    printf("%08x\n", crc);

    return 0;
}