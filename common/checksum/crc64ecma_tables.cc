// an offline generator for crc64-ecma shift tables (L&R) of power-of-two sizes

#include <stdio.h>
#include <inttypes.h>

static uint64_t crctbl[256];
static uint64_t shift_table_power2[64];
static uint64_t ishift_table_power2[64];
static uint64_t POLY = 0xC96C5795D7870F42ull;

// return c ? val : 0 ;
#define CVAL(c, val) (-!!(c) & (val))

void GenTbl(void)                       /* generate crc table */
{
    uint64_t crc, c, i;
    for(c = 0; c < 0x100; c++){
        crc = c;
        for(i = 0; i < 8; i++)
            crc = (crc>>1) ^ CVAL(crc&1, POLY);
        crctbl[c] = crc;
    }
}

uint64_t GenCrc(uint8_t * bfr, size_t size) /* generate crc */
{
    uint64_t crc = 0u;
    while(size--)
        crc = (crc>>8) ^ crctbl[crc & 0xff ^ *bfr++];
    return(crc);
}

/* carryless multiply modulo crc */
uint64_t MpyModCrc(uint64_t a, uint64_t b) /* (a*b)%crc */
{
    uint64_t pd = 0, i;
    for(i = 0; i < 64; i++){
        pd = (pd>>1) ^ CVAL(pd&1, POLY) ^ CVAL(b&1, a);
        b >>= 1;
    }
    return pd;
}

static inline uint64_t crc64_multiply_(uint64_t a, uint64_t b) {
  if ((a ^ (a-1)) < (b ^ (b-1))) {
    uint64_t t = a;
    a = b;
    b = t;
  }

  if (a == 0)
    return 0;

  uint64_t r = 0, h = UINT64_C(1) << 63;
  for (; a != 0; a <<= 1) {
    if (a & h) {
      r ^= b;
      a ^= h;
    }

    b = (b >> 1) ^ ((b & 1) ? POLY : 0);
  }

  return r;
}

uint64_t PowModCrc(uint64_t p);

void gen_shift_table_power2() {
    uint64_t x = 0x1ull << 62;      // shift by 1
    shift_table_power2[0] = x;
    printf("0x%016llx, ", x);
    for (uint32_t i = 1; i < 64; ++i) {
        x = shift_table_power2[i] = crc64_multiply_(x, x);
        printf("0x%016llx, ", x);
        if (i%4 == 3) printf("\n");
    }

    puts("inverse table");

    x = 0x92d8af2baf0e1e85llu;  // x^-1
    ishift_table_power2[0] = x;
    printf("0x%016llx, ", x);
    for (uint32_t i = 1; i < 64; ++i) {
        x = ishift_table_power2[i] = crc64_multiply_(x, x);
        printf("0x%016llx, ", x);
        if (i%4 == 3) printf("\n");
    }

    puts("verification");

    for (uint32_t i = 0; i < 64; ++i) {
        printf("0x%016llx, ", MpyModCrc(shift_table_power2[i],
                                    ishift_table_power2[i]));
        if (i%4 == 3) printf("\n");
    }
}

/* exponentiate by repeated squaring modulo crc */
uint64_t PowModCrc(uint64_t p)          /* pow(2,p)%crc */
{
    uint64_t* table = shift_table_power2;
    uint64_t prd = 0x1ull << 63;                    /* current product */
    for(; p; p &= p-1) {
        uint64_t i = __builtin_ctzll(p);
        // printf("%d, ", i);
        prd = MpyModCrc(prd, table[i]);
    }
    // printf("\n");
    return prd;
}

uint64_t iPowModCrc(uint64_t p)          /* pow(2,p)%crc */
{
    uint64_t* table = ishift_table_power2;
    uint64_t prd = 0x1ull << 63;                    /* current product */
    for(; p; p &= p-1) {
        uint64_t i = __builtin_ctzll(p);
        // printf("%d, ", i);
        prd = MpyModCrc(prd, table[i]);
    }
    // printf("\n");
    return prd;
}

void print_shift_talbes() {
    printf("0x%016llx, 0x%016llx\n", PowModCrc(-1ull), PowModCrc(-1ull-1));
    puts("const static uint32_t crc64ecma_lshift_table_hw[32] = {");
    for (uint32_t i = 0; i < 32; ++i) {
        printf("0x%016llx, ", PowModCrc((1ull << (i+3)) - 1));
        if (i % 4 == 3) puts("");
    }
    puts("};");

    puts("const static uint32_t crc64ecma_rshift_table_hw[32] = {");
    for (uint32_t i = 0; i < 32; ++i) {
        printf("0x%016llx, ", iPowModCrc((1ull << (i+3)) + 1));
        if (i % 4 == 3) puts("");
    }
    puts("};");
}

/*  message 14 data, 18 zeroes */
/*  parities = crc cycled backwards 18 bytes */

int main()
{
    uint64_t pmr, crc, par;
    uint8_t msg[32] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

    GenTbl();                           /* generate crc table */
    const uint32_t num_0 = 15;
    gen_shift_table_power2();
    print_shift_talbes();
    pmr = iPowModCrc(num_0*8ull);         /* pmr = pow(2,-1-18*8)%crc */
    crc = GenCrc(msg, 14+num_0);              /* generate crc including 18 zeroes */
    par = MpyModCrc(crc, pmr);          /* par = (crc*pmr)%crc = new crc */
    crc = GenCrc(msg, 14);              /* generate crc for shortened msg */
    printf("%016llx %016llx\n", par, crc);    /* crc == par */
    *(uint64_t*)&msg[14] = par;
    crc = GenCrc(msg, 22);              /* crc == 0 */
    printf("%016llx\n", crc);

    return 0;
}