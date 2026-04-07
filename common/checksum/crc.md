Basic Mathematics on Fast CRC Calculation
====================

# Definitions

M: original text

T: bit-width of a CRC scheme, e.g. 16, 32, 64, etc.

P: the polynomial of a CRC scheme, which is inherently a (T+1)-bit polynomial, and the most significant bit (MSB) is often omitted for convenience.

CRC(M): treat M as a large binary number of $len(M)*8$ bits, can calculate $M * x^T \mod P$ in $GF(2^T)$. That is:
```
  CRC(M)
= M * x^T mod P
= M000..0 mod P  # padding T trailing 0s to M
```

$GF(2^T)$: Galois Field, where ```*``` is carryless multiplication (polynomial multiplication); both ```+``` and ```-``` are ```xor```.

Corollary 1: CRC is linear
```
CRC(A) + CRC(B) = CRC(A + B)
```

Corollary 2: Padding 0 to the front doesn't change CRC
```
CRC(A) = CRC(00...0A)
```

# CRC Shifting
Given CRC(M), how to calculate CRC(M<<n)?
```
  CRC(M<<n)
= CRC(M * x^n)
= M * x^n * x^T mod P
= CRC(M) * x^n mod P
= CRC(M) * (x^n mod P) mod P
```
where ```x^n mod P``` is a pre-computed constant. Actually we can pre-compute all ```x^(2^i) mod P``` for i == 1, 2, 3, etc., and decompose n into power(s) of 2, then apply the calculation(s) accordingly.

We have 2 ways to calculate the ```mod P``` after multiplication.

First, if we have a hardware instruction (or other fast approach) to calculate CRC from 2*T bits of data:
```
  CRC(M) * (x^n mod P) mod P
= CRC(M) * [x^(n-T) mod P] * x^n mod P
= CRC(CRC(M) * [x^(n-T) mod P])
```

Otherwise, we make use of Barrett reduction:
```
let M' = CRC(M) * [x^n mod P]
let // be an operator to compute the "integer" part of the quotient, ignoring the remainder.

  M' mod P
= M' - M' // P * P
= M' - M' * [x^(2T) // P] * [P // x^(2T)]
= M' - M' * [x^(2T) // P] // x^T * P // x^T
```
Where ```x^128 // P``` is a pre-computed constant, named ```Mu```, so
```
  M' - M' * [x^(2T) // P] // x^T * P // x^T
= M' + M' * Mu // x^T * P // x^T
= M' + [(M' * Mu >> T) * P >> T]
```

# CRC Inversed Shifting
Given CRC(M), and M has at least n trailing 0s. how to calculate CRC(M<<n)?
```
  CRC(M>>n)
= CRC(M * x^-n)
= M * x^-n * x^T mod P
= CRC(M) * x^-n mod P
= CRC(M) * (x^-n mod P) mod P
```
where ```x^-n mod P``` is a pre-computed constant. Actually we can pre-compute all ```(x^-1)^(2^i) mod P``` for i == 1, 2, 3, etc., and at runtime decompose n into power(s) of 2, then apply the calculation(s) accordingly.

If P is **irreducible** (cannot be factored into lower-degree polynomials),
we have ```x^-k = x^(2^T-1-k) mod P```, so ```x^-1 = x^(2^T-2) mod P```.
This is Fermat's Little Theorem, and we can transform the equation into:
```
  x^-1 mod P
= x^(2^T-2) mod P    # if P is irreducible
= x^1 * x^2 * x^4 * ... * x^(T-1) mod P
```
This can be obtained by repeated self-squaring and multiplication.

If P is **reducible** otherwise, we should use Extended Euclidean Algorithm
to compute ```x^-1 mod P```.

Once we have ```x^-1 mod P```, we can easily pre-compute all
```(x^-1)^(2^i) mod P``` for i == 1, 2, 3, etc. by applying self-squaring.

# Fast CRC32C Calculation with Shifting
X86 has CRC32C instructions since SSE 4.1. They have an latency of 3 cycles,
while they can be issued every cycle can executed in a pipeline, as long as
there is no data dependence.

So the better approach to CRC32C instructions is, dividing input data into 3
portions, then calculate individual CRC32C values of these portions in an
**interleaved** manner, and join these values together. For example, suppose
there is a message of "abcdQWERwxyz". We calculate the CRC32C value in the
following order:
```
c1 = CRC32C(0, 'a');
c2 = CRC32C(0, 'Q');
c3 = CRC32C(0, 'w');

c1 = CRC32C(c1, 'b');
c2 = CRC32C(c2, 'W');
c3 = CRC32C(c3, 'x');

c1 = CRC32C(c1, 'c');
c2 = CRC32C(c2, 'E');
c3 = CRC32C(c3, 'y');

c1 = CRC32C(c1, 'c');
c2 = CRC32C(c2, 'R');
c3 = CRC32C(c3, 'y');

return shift(c1, 8*8) xor shift(c2, 4*8) xor c3;
```

# Fast CRC Calculation with ```clmul```
Modern processors usually provide vectorized ```clmul``` instructions for
64-bit carryless multiplication, which can be used for fast CRC Calculation.
First, we treat the original text ```M``` as a 256-bit header ```H``` and an
arbitrary trailer ```G```.

```
  CRC(M)
= CRC((H * x^len(G) + G))
= CRC(H) * x^len(G) mod P + CRC(G)
```

And G can be further splitted into 4 64-bit parts H1, H2, H3, and H4. So
```
  CRC(H)
= CRC(H1 * x^192 + H2 * x^128 + H3 * x^64 + H4)
= (H1 * x^192 + H2 * x^128 + H3 * x^64 + H4) * x^T mod P
= [H1 * (x^192 mod P) + H2 * (x^128 mod P) + H3H4] * x^T mod P  # treat H3H4 as a single 128-bit data
= CRC(H1 * (x^192 mod P) + H2 * (x^128 mod P) + H3H4)
= CRC(H')  # assuming H' = H1 * (x^192 mod P) + H2 * (x^128 mod P) + H3H4
```
Thus the CRC value of a 256-bit H equals to that of a 128-bit H', and:
```
  CRC(M)
= CRC(H) * x^len(G) mod P + CRC(G)
= CRC(H') * x^len(G) mod P + CRC(G)
= CRC(H'G)
= CRC(M')  # the header part of M' has been folded by 128-bit
```
We have successfully fold the data by 128-bit, and we can repeatedly apply
the trick until the length of transformed text is < 256 (and it is >= 128).
We then pad the data with (most-significant) zeros to 256 bits and then
apply another single fold to generate a 128-bit buffer.

Even if we have AVX-256 and AVX-512 on modern x86 processors, the
```clmul``` operation in each lane is till 64-bit * 64-bit obtaining
a 127-bit result. We can still make use the wider operation with a
similar trick to fold 512-bit data in each operation.


## Final Reduction of 128-bit for CRC32
Suppose the final data is M128, so ```CRC32(M128) = M128 * x^32 mod P```, which
is effectively 160-bit data. We can fold the highest 64-bit with ```clmul```
and obtain a 96-bit data, which is then xor-ed with the remaining 96-bit
data, resulting 96-bit data.

We then fold following 32-bit with ```clmul``` and obtain a 64-bit data,
which is then xor-ed with the remaining 64-bit data, resulting final
64-bit data.

We finally apply Barrett reduction to the 64-bit data.

## Final Reduction of 128-bit for CRC64
Suppose the final data is M128, so ```CRC64(M128) = M128 * x^64 mod P```, which
is effectively 192-bit data. We can fold the highest 64-bit with ```clmul```
and obtain a 128-bit data, which is then xor-ed with the remaining 128-bit
data, resulting 128-bit data.

We finally apply Barrett reduction to the 128-bit data.

# Bit-Reflection
In computer engineering, we usually treat the 1st byte as the most significant
byte, and the lowest bit as the most significant bit, for ease of calculation.
This is called bit-reflection. Many CRC schemes take this format, such as
CRC32C, CRC64-ECMA, etc.

Carryless multiplication of bit-reflected data produces results bigger than
true result by ```x```. For example, think about ```1 * 1``` that represents
```x^63 * x63```; it results in ```1``` that represents ```x^127```, but it
actually should be ```x^126```. We can either fix the result by shifting 1
bit, or provide a operand constant that is smaller by ```x``` in advance.

